// ratcast-recv: connect to a sender, play its video in a window (or without
// one) and its sound, or write out its MPEG-TS feed, and print a line of stats
// a second and, on exit, a summary line.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "common/arguments.h"
#include "common/tool.h"
#include "ratcast/client/call.h"
#include "ratcast/client/call_monitor.h"
#include "ratcast/client/cpu_meter.h"
#include "ratcast/client/playback_meter.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/ts_io.h"
#include "ratcast/engine/virtual_output.h"

#ifdef RATCAST_TOOLS_WINDOW
#include "recv/video_window.h"
#endif

using namespace ratcast;
using namespace ratcast::tools;
using namespace std::chrono_literals;

namespace {

/// Where this tool's own lines go: standard output, unless the feed does.
FILE* g_out = stdout;

double ms(Duration d) { return double(d.count()) / 1000; }

constexpr const char* kUsage = R"(usage: ratcast-recv rats://HOST:PORT | rats://PEER-ID [options]

  rats://HOST:PORT     the sender's address (or --connect HOST:PORT)
  rats://PEER-ID       the sender's peer id, found through the DHT, if it runs --announce
                       (or --peer PEER-ID)
  --output URL         write the sender's MPEG-TS feed out: udp://host:port, - for
                       standard output (this tool's own lines go to standard error then),
                       or a file
  --headless           decode, but show nothing
  --audio speaker|virtual|off
                       where the sound goes, if the sender sends any: the default
                       output, or nowhere but played all the same (the default with
                       --headless), or not received at all
  --no-sync            play the sound and the pictures each in its own time, not in step
  --buffer MS          hold pictures back at most this long to play evenly past late
                       frames (default: the stream's frame lifetime; 0: show each at once);
                       a feed, this long past its fastest (default: its lifetime)
  --frames N           exit once N pictures are decoded
  --duration S         exit after S seconds
  --data-dir DIR       keep the peer id across runs
  --verbose            library logs

The pattern source's pictures carry the time of day they were captured: on
the sender's own host, or one whose clock is synchronised with it (NTP), the
latency from capture to picture, as played (and as presented), is printed too;
so is the tone's, from capture to the speaker.
On exit a summary line, key=value, sums up the run.
)";

struct Options {
    std::optional<RatsUrl> sender;
    bool headless = false;
    std::string output;  ///< where a feed goes; none: not received
    std::string audio;
    bool sync = true;
    std::optional<Duration> buffer;
    uint64_t frames = 0;
    uint32_t duration = 0;
    std::string data_dir;
    bool verbose = false;
};

std::optional<Options> parse(int argc, char** argv) {
    Arguments args(argc, argv);
    if (args.flag("help")) {
        std::cout << kUsage;
        std::exit(0);
    }
    Options o;
    bool ok = true;
    const auto sender = [&](std::optional<RatsUrl> url, const std::string& text) {
        if (url && !o.sender) {
            o.sender = std::move(url);
        } else {
            std::cerr << "ratcast-recv: '" << text << "' is no sender, or one too many\n";
            ok = false;
        }
    };
    if (const auto connect = args.text("connect")) {
        std::optional<librats::HostEndpoint> endpoint = librats::HostEndpoint::parse(*connect);
        sender(endpoint ? std::optional<RatsUrl>(std::move(*endpoint)) : std::nullopt, *connect);
    }
    if (const auto peer = args.text("peer")) {
        const std::optional<PeerId> id = PeerId::from_hex(*peer);
        sender(id ? std::optional<RatsUrl>(*id) : std::nullopt, *peer);
    }
    o.headless = args.flag("headless");
    o.sync = !args.flag("no-sync");
    o.audio = args.text("audio").value_or(o.headless ? "virtual" : "speaker");
    if (o.audio != "speaker" && o.audio != "virtual" && o.audio != "off") {
        std::cerr << "ratcast-recv: --audio wants speaker, virtual or off\n";
        ok = false;
    }
    if (const auto buffer = args.number("buffer", 0, 10'000)) {
        o.buffer = std::chrono::milliseconds(*buffer);
    }
    o.frames = args.number("frames", 1, 1'000'000'000).value_or(0);
    o.duration = args.number("duration", 1, 1'000'000).value_or(0);
    o.data_dir = args.text("data-dir").value_or("");
    o.verbose = args.flag("verbose");
    o.output = args.text("output").value_or("");
    for (const std::string& url : args.positional()) sender(parse_rats_url(url), url);
    for (const std::string& e : args.errors()) {
        std::cerr << "ratcast-recv: " << e << "\n";
        ok = false;
    }
    if (ok && !o.sender) {
        std::cerr << "ratcast-recv: say where the sender is: rats://HOST:PORT or rats://PEER-ID\n";
        ok = false;
    }
    return ok ? std::optional(o) : std::nullopt;
}

const char* kind_name(MediaKind kind) {
    switch (kind) {
        case MediaKind::Video: return "stream";
        case MediaKind::Audio: return "sound";
        case MediaKind::Data: return "feed";
    }
    return "stream";
}

/// What started playing, or why it could not.
void print_playing(const PeerId& peer, const StreamDescription& stream, Result<void> outcome,
                   std::optional<Duration> buffer) {
    if (!outcome) {
        std::cerr << "cannot play " << kind_name(stream.kind()) << " '" << stream.label
                  << "': " << to_string(outcome.error()) << "\n";
        return;
    }
    if (const VideoFormat* format = stream.video()) {
        std::fprintf(g_out, "playing '%s' from %s: %ux%u\n", stream.label.c_str(),
                     peer.short_hex().c_str(), format->width, format->height);
    } else if (const AudioFormat* format = stream.audio()) {
        std::fprintf(g_out, "playing sound '%s' from %s: %u Hz, %u channel(s)\n",
                     stream.label.c_str(), peer.short_hex().c_str(), format->sample_rate,
                     unsigned(format->channels));
    } else {
        std::fprintf(g_out, "writing feed '%s' from %s out, %.0f ms behind\n", stream.label.c_str(),
                     peer.short_hex().c_str(), ms(buffer.value_or(stream.lifetime)));
    }
}

/// Latency of a picture from its capture, if it carries the pattern's stamp
/// and that stamp makes sense here: the sender's clock may be another's.
std::optional<Duration> latency_of(const VideoFrame& picture) {
    const std::optional<Timestamp> stamp = read_timestamp(picture);
    if (!stamp) return std::nullopt;
    const Duration latency = WallClock::instance().now() - *stamp;
    if (latency < Duration::zero() || latency > std::chrono::seconds(10)) return std::nullopt;
    return latency;
}

/// Latency of a frame of sound from its capture to the speaker, if its
/// capture time is the time of day (the tone's) and makes sense here.
std::optional<Duration> latency_of(Timestamp capture_time, Timestamp play_time) {
    const Duration latency =
        WallClock::instance().now() - SteadyClock::instance().now() + (play_time - capture_time);
    if (latency < Duration::zero() || latency > std::chrono::seconds(10)) return std::nullopt;
    return latency;
}

/// A report a second, from the call's monitor, with the latency the stamps
/// tell; a summary at the end.
class Reporter {
public:
    Reporter(Call& call, PlaybackMeter& decoded, PlaybackMeter* shown, PlaybackMeter& heard)
        : call_(call), monitor_(call), decoded_(decoded), shown_(shown), heard_(heard) {}

    void report() {
        const CallReport report = monitor_.report();
        if (report.video || report.sound || report.feed) {
            std::fputs(report_text(report, seconds_++).c_str(), g_out);
        } else {
            std::fprintf(g_out, "[%4us] waiting for a stream\n", seconds_++);
        }
        report_latency();
        // The last word on each, for a summary after it ended.
        if (const auto video = call_.video_stats()) {
            last_player_ = video->player;
            last_stream_ = video->stream;
        }
        if (const auto sound = call_.sound_stats()) last_sound_ = *sound;
        if (const auto feed = call_.feed_stats()) last_feed_ = *feed;
        std::fflush(g_out);
    }

    /// The whole run as key=value pairs, for scripts. Call before the call ends.
    void summarise() {
        std::fprintf(g_out, "summary cpu=%.0f", cpu_.total());
        summarise_video();
        summarise_feed();
        summarise_sound();
        std::fprintf(g_out, "\n");
        std::fflush(g_out);
    }

    /// Whether anything was played: pictures, sound, or a feed written out.
    bool played() {
        const std::optional<DataPlayerStats> feed = call_.feed_stats();
        return decoded_.total().pictures > 0 || heard_.total().pictures > 0 ||
               (feed ? feed->bytes : last_feed_.bytes) > 0;
    }

private:
    /// Capture to picture — as played, and as presented — and to the
    /// speaker, where the stamps make sense here.
    void report_latency() {
        const PlaybackStats decoded = decoded_.take_interval();
        const std::optional<PlaybackStats> shown =
            shown_ ? std::optional(shown_->take_interval()) : std::nullopt;
        const PlaybackStats heard = heard_.take_interval();
        if (decoded.latency.count() == 0 && heard.latency.count() == 0) return;
        std::fprintf(g_out, "        %-14s", "latency");
        print_latency("pictures", decoded.latency);
        if (shown) print_latency("shown", shown->latency);
        print_latency("sound", heard.latency);
        std::fprintf(g_out, "\n");
    }

    void summarise_feed() {
        const DataPlayerStats feed = call_.feed_stats().value_or(last_feed_);
        if (feed.delivery.received == 0) return;
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started_;
        std::fprintf(g_out,
                     " data_frames=%llu data_lost=%llu data_late=%llu data_early=%llu"
                     " data_mbps=%.2f data_buffer_ms=%.0f",
                     static_cast<unsigned long long>(feed.delivery.delivered),
                     static_cast<unsigned long long>(feed.delivery.lost),
                     static_cast<unsigned long long>(feed.delivery.late),
                     static_cast<unsigned long long>(feed.delivery.early),
                     double(feed.bytes) * 8 / 1e6 / std::max(elapsed.count(), 1.0),
                     ms(feed.latency));
    }

    void summarise_video() {
        const PlaybackStats decoded = decoded_.total();
        const auto stats = call_.video_stats();
        if (!stats && last_stream_.bytes == 0) return;  // no pictures offered
        const VideoPlayerStats player = stats ? stats->player : last_player_;
        const IncomingStreamStats stream = stats ? stats->stream : last_stream_;
        const double first = decoded.first_picture ? ms(*decoded.first_picture) : -1;
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started_;
        // From the first picture on: what the viewer was shown, per second.
        const double playing = elapsed.count() - std::max(first, 0.0) / 1000;
        std::fprintf(g_out,
                     " first_picture_ms=%.0f pictures=%llu stalls=%llu stalled_ms=%.0f"
                     " longest_gap_ms=%.0f lost=%llu late=%llu dropped=%llu keyframe_requests=%llu"
                     " fps=%.1f mbps=%.2f buffer_ms=%.0f",
                     first, static_cast<unsigned long long>(decoded.pictures),
                     static_cast<unsigned long long>(decoded.stalls), ms(decoded.stalled),
                     ms(decoded.longest_gap), static_cast<unsigned long long>(stream.frames.lost),
                     static_cast<unsigned long long>(stream.frames.late),
                     static_cast<unsigned long long>(dropped(player, stream)),
                     static_cast<unsigned long long>(stream.frames.keyframe_requests +
                                                     player.keyframe_requests),
                     playing > 0 ? double(decoded.pictures) / playing : 0,
                     double(stream.bytes) * 8 / 1e6 / std::max(elapsed.count(), 1.0),
                     ms(player.delay));
        print_percentiles("latency", decoded.latency);
        if (shown_) print_percentiles("shown", shown_->total().latency);
    }

    /// Breaks: gaps of 100 ms or more between frames of sound played, all
    /// made up; a single lost frame is not one.
    void summarise_sound() {
        const PlaybackStats heard = heard_.total();
        const std::optional<AudioPlayerStats> stats = call_.sound_stats();
        if (!stats && heard.pictures == 0) return;
        const AudioPlayerStats sound = stats.value_or(last_sound_);
        const JitterBuffer::Stats& jitter = sound.jitter;
        std::fprintf(
            g_out,
            " first_sound_ms=%.0f audio_frames=%llu audio_lost=%llu audio_late=%llu"
            " audio_dropped=%llu audio_recovered_ms=%.0f audio_concealed_ms=%.0f"
            " audio_expanded_ms=%.0f audio_cut_ms=%.0f audio_breaks=%llu audio_delay_ms=%.0f"
            " audio_target_ms=%.0f",
            heard.first_picture ? ms(*heard.first_picture) : -1,
            static_cast<unsigned long long>(jitter.played),
            static_cast<unsigned long long>(jitter.lost),
            static_cast<unsigned long long>(jitter.late),
            static_cast<unsigned long long>(jitter.dropped), ms(sound.decoded.recovered),
            ms(sound.decoded.concealed), ms(jitter.expanded), ms(jitter.cut),
            static_cast<unsigned long long>(heard.stalls), ms(sound.delay), ms(sound.target));
        print_percentiles("audio_latency", heard.latency);
        const PlayoutClockStats sync = call_.sync_stats();
        if (sync.apart.count() == 0) return;
        print_percentiles("av_apart", sync.apart);
        std::fprintf(g_out, " av_apart_signed_mean_ms=%.1f", ms(sync.mean_apart));
    }

    /// Frames that reached this host and were never shown.
    static uint64_t dropped(const VideoPlayerStats& player, const IncomingStreamStats& stream) {
        return stream.frames.undecodable + player.dropped;
    }

    static void print_latency(const char* name, const LatencyHistogram& latency) {
        if (latency.count() == 0) return;
        std::fprintf(g_out, "  %s %5.1f ms (p95 %5.1f, worst %5.1f)", name, ms(latency.mean()),
                     ms(latency.percentile(0.95)), ms(latency.max()));
    }

    static void print_percentiles(const char* name, const LatencyHistogram& latency) {
        if (latency.count() == 0) return;
        std::fprintf(g_out,
                     " %s_mean_ms=%.1f %s_p50_ms=%.1f %s_p95_ms=%.1f %s_p99_ms=%.1f %s_max_ms=%.1f",
                     name, ms(latency.mean()), name, ms(latency.percentile(0.5)), name,
                     ms(latency.percentile(0.95)), name, ms(latency.percentile(0.99)), name,
                     ms(latency.max()));
    }

    Call& call_;
    CallMonitor monitor_;
    PlaybackMeter& decoded_;
    PlaybackMeter* const shown_;
    PlaybackMeter& heard_;
    CpuMeter cpu_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    VideoPlayerStats last_player_;
    IncomingStreamStats last_stream_;
    AudioPlayerStats last_sound_;
    DataPlayerStats last_feed_;
    unsigned seconds_ = 1;
};

}  // namespace

int main(int argc, char** argv) {
    const std::optional<Options> options = parse(argc, argv);
    if (!options) {
        std::cerr << "\n" << kUsage;
        return 2;
    }
    const Options& o = *options;
    set_verbose(o.verbose);
    catch_interrupts();

    // Where a feed goes, if one is wanted: no window then.
    std::unique_ptr<TsOutput> feed;
    if (!o.output.empty()) {
        if (o.output == "-") g_out = stderr;
        Result<std::unique_ptr<TsOutput>> opened = open_ts_output(o.output);
        if (!opened) {
            std::cerr << "ratcast-recv: cannot write to " << o.output << ": "
                      << to_string(opened.error()) << "\n";
            return 1;
        }
        feed = std::move(*opened);
    }
    const bool headless = o.headless || feed;

#ifdef RATCAST_TOOLS_WINDOW
    std::unique_ptr<VideoWindow> window;
    if (!headless) {
        window = VideoWindow::open("ratcast");
        if (!window) return 1;
    }
#else
    if (!headless) std::cerr << "built without SDL3: no window, as with --headless\n";
#endif

    // Where the sound goes: the speaker, or a device of our own that plays
    // it in real time to nobody.
    std::unique_ptr<AudioOutput> output;
    const AudioFormat sound_format{48'000, 2};
    if (o.audio == "speaker") {
        Result<std::unique_ptr<AudioOutput>> speaker = open_audio_output({.format = sound_format});
        if (speaker) {
            output = std::move(*speaker);
        } else {
            std::cerr << "no sound output (" << to_string(speaker.error())
                      << "): the sound plays to nobody\n";
        }
    }
    if (!output && o.audio != "off") {
        output = std::make_unique<VirtualOutput>(VirtualOutputConfig{.format = sound_format});
    }

    // Pictures as the player hands them on, on its schedule, and, in a
    // window, as presented; frames of sound as they go to the output. The
    // wait for the first one counts from the moment we dial.
    PlaybackMeter decoded;
    std::optional<PlaybackMeter> shown;
#ifdef RATCAST_TOOLS_WINDOW
    if (window) shown.emplace();
#endif
    PlaybackMeter heard;
    const CallPlayback playback{
        .pictures =
            [&](VideoFrame picture) {
                decoded.picture(latency_of(picture));
#ifdef RATCAST_TOOLS_WINDOW
                if (window) window->show(std::move(picture));
#endif
            },
        .video = {.max_delay = o.buffer},
        .sound = output.get(),
        .heard = [&](Timestamp capture_time,
                     Timestamp play_time) { heard.picture(latency_of(capture_time, play_time)); },
        .sync = o.sync,
        .feed = feed.get(),
        .feed_config = {.latency = o.buffer},
    };
    const CallHandlers handlers{
        .playing = [&](const PeerId& peer, const StreamDescription& stream,
                       Result<void> outcome) { print_playing(peer, stream, outcome, o.buffer); },
        .ended =
            [](const PeerId&, const StreamDescription& stream) {
                std::fprintf(g_out, "the %s ended\n", kind_name(stream.kind()));
            },
    };

    decoded.start();
    if (shown) shown->start();
    heard.start();
    Result<std::unique_ptr<Call>> started = Call::start(
        {.listen = false, .data_dir = o.data_dir, .dial = o.sender, .playback = playback},
        handlers);
    if (!started) {
        std::cerr << "ratcast-recv: the node did not start\n";
        return 1;
    }
    Call& call = **started;
    if (const auto* endpoint = std::get_if<librats::HostEndpoint>(&*o.sender)) {
        std::fprintf(g_out, "connecting to %s\n", endpoint->to_string().c_str());
    } else {
        std::fprintf(g_out, "looking for %s through the DHT (can take a minute)\n",
                     std::get<PeerId>(*o.sender).short_hex().c_str());
    }

    Reporter reporter(call, decoded, shown ? &*shown : nullptr, heard);
    const auto start = std::chrono::steady_clock::now();
    auto next = start + 1s;
    const auto keep_going = [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            next += 1s;
            reporter.report();
        }
        if (o.frames > 0 && decoded.total().pictures >= o.frames) return false;
        if (o.duration > 0 && now - start >= std::chrono::seconds(o.duration)) return false;
        return !interrupted();
    };
#ifdef RATCAST_TOOLS_WINDOW
    if (window) {
        window->run(keep_going,
                    [&](const VideoFrame& picture) { shown->picture(latency_of(picture)); });
    } else
#endif
    {
        while (keep_going()) std::this_thread::sleep_for(20ms);
    }

    reporter.summarise();
    const bool played = reporter.played();
    started->reset();
    return played ? 0 : 1;
}
