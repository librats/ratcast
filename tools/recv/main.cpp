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
#include "ratcast/client/audio_player.h"
#include "ratcast/client/data_player.h"
#include "ratcast/client/playback_meter.h"
#include "ratcast/client/video_player.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/ts_io.h"
#include "ratcast/engine/virtual_output.h"
#include "ratcast/session/media_service.h"

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
    std::optional<std::pair<std::string, uint16_t>> connect;
    std::optional<PeerId> peer;
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
    if (const auto connect = args.text("connect")) {
        o.connect = parse_endpoint(*connect);
        if (!o.connect) {
            std::cerr << "ratcast-recv: --connect wants HOST:PORT\n";
            ok = false;
        }
    }
    if (const auto peer = args.text("peer")) {
        o.peer = PeerId::from_hex(*peer);
        if (!o.peer) {
            std::cerr << "ratcast-recv: --peer wants the sender's peer id (hex)\n";
            ok = false;
        }
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
    for (const std::string& url : args.positional()) {  // after every flag
        const std::optional<SenderAddress> address = parse_rats_url(url);
        if (!address || o.connect || o.peer) {
            std::cerr << "ratcast-recv: '" << url << "' is no rats:// URL of a sender\n";
            ok = false;
            continue;
        }
        o.connect = address->endpoint;
        o.peer = address->peer;
    }
    for (const std::string& e : args.errors()) {
        std::cerr << "ratcast-recv: " << e << "\n";
        ok = false;
    }
    if (ok && !o.connect && !o.peer) {
        std::cerr << "ratcast-recv: say where the sender is: rats://HOST:PORT or rats://PEER-ID\n";
        ok = false;
    }
    return ok ? std::optional(o) : std::nullopt;
}

struct ViewerConfig {
    std::optional<PeerId> sender;  ///< none: whoever connects
    VideoPlayerConfig video;
    VideoPlayer::Sink pictures;
    AudioOutput* sound = nullptr;  ///< none: sound is not received
    AudioPlayer::Observer heard;
    bool sync = true;          ///< the sound and the pictures in step
    TsOutput* feed = nullptr;  ///< none: a feed is not received
    DataPlayerConfig feed_config;
};

/// Plays the first video stream the sender offers, its first audio stream and
/// its first feed, each where the config says, and the next ones when they go
/// and come back.
class Viewer {
public:
    explicit Viewer(ViewerConfig config)
        : config_(std::move(config)), sync_(SteadyClock::instance(), config_.sync) {
        // Pictures and sound always tell the clock how they play, which
        // measures how far apart they are; only in step do they follow it.
        config_.video.sync = &sync_;
    }

    SessionHandlers handlers() {
        return SessionHandlers{
            .stream_announced = [this](const PeerId& peer,
                                       const StreamDescription& d) { announced(peer, d); },
            .stream_withdrawn = [this](const PeerId& peer, StreamId s) { withdrawn(peer, s); },
            .frame_received =
                [this](const PeerId& peer, EncodedFrame frame) {
                    std::lock_guard lock(mutex_);
                    if (playing(player_, peer, frame.info.stream)) {
                        player_->push(std::move(frame));
                    } else if (playing(sound_, peer, frame.info.stream)) {
                        sound_->push(std::move(frame));
                    } else if (playing(feed_, peer, frame.info.stream)) {
                        feed_->push(std::move(frame));
                    }
                },
        };
    }

    void attach(MediaSession& session) { session_ = &session; }

    void stop() {
        std::lock_guard lock(mutex_);
        player_.reset();
        sound_.reset();
        feed_.reset();
        ended_feed_.reset();
    }

    /// What is playing: its player's and its stream's stats.
    std::optional<std::pair<VideoPlayerStats, IncomingStreamStats>> stats() {
        std::lock_guard lock(mutex_);
        if (!player_) return std::nullopt;
        const std::optional<IncomingStreamStats> stream =
            session_->incoming_stats(player_->peer(), player_->stream());
        if (!stream) return std::nullopt;
        return std::pair{player_->stats(), *stream};
    }

    std::optional<AudioPlayerStats> sound_stats() {
        std::lock_guard lock(mutex_);
        if (!sound_) return std::nullopt;
        return sound_->stats();
    }

    std::optional<DataPlayerStats> feed_stats() {
        std::lock_guard lock(mutex_);
        const std::unique_ptr<DataPlayer>& feed = feed_ ? feed_ : ended_feed_;
        if (!feed) return std::nullopt;
        return feed->stats();
    }

    /// Sound and pictures: how far apart they played, and what each needs.
    PlayoutClockStats sync_stats() const { return sync_.stats(); }

private:
    template <typename Player>
    static bool playing(const std::unique_ptr<Player>& player, const PeerId& peer,
                        StreamId stream) {
        return player && player->peer() == peer && player->stream() == stream;
    }

    void announced(const PeerId& peer, const StreamDescription& description) {
        std::lock_guard lock(mutex_);
        if (playing(player_, peer, description.id)) {
            player_->update(description);
            return;
        }
        if (config_.sender && peer != *config_.sender) return;
        switch (description.kind()) {
            case MediaKind::Video:
                if (!player_) play_pictures(peer, description);
                return;
            case MediaKind::Audio:
                if (!sound_ && config_.sound) play_sound(peer, description);
                return;
            case MediaKind::Data:
                if (!feed_ && config_.feed) play_feed(peer, description);
                return;
        }
    }

    void play_pictures(const PeerId& peer, const StreamDescription& description) {
        auto player = std::make_unique<VideoPlayer>(*session_, peer, description, config_.pictures,
                                                    config_.video);
        if (Result<void> started = player->start(); !started) {
            std::cerr << "cannot play stream '" << description.label
                      << "': " << to_string(started.error()) << "\n";
            return;
        }
        const VideoFormat* format = description.video();
        std::fprintf(g_out, "playing '%s' from %s: %ux%u\n", description.label.c_str(),
                     peer.short_hex().c_str(), format->width, format->height);
        player_ = std::move(player);
    }

    void play_sound(const PeerId& peer, const StreamDescription& description) {
        auto sound =
            std::make_unique<AudioPlayer>(*session_, peer, description, *config_.sound,
                                          AudioPlayerConfig{.sync = &sync_}, config_.heard);
        if (Result<void> started = sound->start(); !started) {
            std::cerr << "cannot play sound '" << description.label
                      << "': " << to_string(started.error()) << "\n";
            return;
        }
        const AudioFormat* format = description.audio();
        std::fprintf(g_out, "playing sound '%s' from %s: %u Hz, %u channel(s)\n",
                     description.label.c_str(), peer.short_hex().c_str(), format->sample_rate,
                     unsigned(format->channels));
        sound_ = std::move(sound);
    }

    void play_feed(const PeerId& peer, const StreamDescription& description) {
        auto feed = std::make_unique<DataPlayer>(*session_, peer, description, *config_.feed,
                                                 config_.feed_config);
        if (Result<void> started = feed->start(); !started) {
            std::cerr << "cannot play feed '" << description.label
                      << "': " << to_string(started.error()) << "\n";
            return;
        }
        std::fprintf(g_out, "writing feed '%s' from %s out, %.0f ms behind\n",
                     description.label.c_str(), peer.short_hex().c_str(),
                     ms(feed->stats().latency));
        feed_ = std::move(feed);
    }

    void withdrawn(const PeerId& peer, StreamId stream) {
        std::lock_guard lock(mutex_);
        if (playing(player_, peer, stream)) {
            std::fprintf(g_out, "the stream ended\n");
            player_.reset();
        } else if (playing(sound_, peer, stream)) {
            std::fprintf(g_out, "the sound ended\n");
            sound_.reset();
        } else if (playing(feed_, peer, stream)) {
            std::fprintf(g_out, "the feed ended\n");
            // What it holds is still to be written out, each at its time.
            feed_->finish();
            ended_feed_ = std::move(feed_);
        }
    }

    ViewerConfig config_;
    PlayoutClock sync_;
    MediaSession* session_ = nullptr;
    std::mutex mutex_;
    std::unique_ptr<VideoPlayer> player_;
    std::unique_ptr<AudioPlayer> sound_;
    std::unique_ptr<DataPlayer> feed_;
    std::unique_ptr<DataPlayer> ended_feed_;  ///< writing out what it held
};

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

/// One line a second, and a summary at the end.
class Reporter {
public:
    Reporter(Viewer& viewer, PlaybackMeter& decoded, PlaybackMeter* shown, PlaybackMeter& heard)
        : viewer_(viewer), decoded_(decoded), shown_(shown), heard_(heard) {}

    void report() {
        std::fprintf(g_out, "[%4us]", seconds_++);
        const bool pictures = report_video();
        const std::optional<DataPlayerStats> feed = viewer_.feed_stats();
        if (feed) report_feed(*feed);
        if (!pictures && !feed && !viewer_.sound_stats())
            std::fprintf(g_out, " waiting for a stream");
        std::fprintf(g_out, "  cpu %3.0f%%\n", cpu_.take_interval());
        report_sound();
        std::fflush(g_out);
    }

    /// The whole run as key=value pairs, for scripts. Call before the viewer stops.
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
        const std::optional<DataPlayerStats> feed = viewer_.feed_stats();
        return decoded_.total().pictures > 0 || heard_.total().pictures > 0 ||
               (feed ? feed->bytes : last_feed_.bytes) > 0;
    }

private:
    /// Whether there are pictures to speak of.
    bool report_video() {
        const PlaybackStats decoded = decoded_.take_interval();
        const std::optional<PlaybackStats> shown =
            shown_ ? std::optional(shown_->take_interval()) : std::nullopt;
        const auto stats = viewer_.stats();
        if (!stats) return false;
        const auto& [player, stream] = *stats;
        const uint64_t pictures = player.pictures - last_player_.pictures;
        const double mbps = double(stream.bytes - last_stream_.bytes) * 8 / 1e6;
        const double decode_ms =
            pictures > 0 ? ms(player.decode_time - last_player_.decode_time) / double(pictures) : 0;
        std::fprintf(g_out,
                     " %3llu fps %6.2f Mbit/s  %ux%u %s  decode %5.2f ms  lost %llu  late %llu"
                     "  dropped %llu  stalls %llu  buffer %3.0f ms",
                     static_cast<unsigned long long>(pictures), mbps, player.width, player.height,
                     player.decoder.c_str(), decode_ms,
                     static_cast<unsigned long long>(stream.frames.lost - last_stream_.frames.lost),
                     static_cast<unsigned long long>(stream.frames.late - last_stream_.frames.late),
                     static_cast<unsigned long long>(dropped(player, stream) -
                                                     dropped(last_player_, last_stream_)),
                     static_cast<unsigned long long>(decoded.stalls), ms(player.delay));
        print_latency("latency", decoded.latency);
        if (shown) print_latency("shown", shown->latency);
        last_player_ = player;
        last_stream_ = stream;
        return true;
    }

    /// What was written out, and what of it the network could not bring in time.
    void report_feed(const DataPlayerStats& feed) {
        const DeliveryBuffer::Stats& now = feed.delivery;
        const DeliveryBuffer::Stats& last = last_feed_.delivery;
        std::fprintf(g_out, " feed %6.2f Mbit/s  lost %llu  late %llu  buffer %3.0f ms",
                     double(feed.bytes - last_feed_.bytes) * 8 / 1e6,
                     static_cast<unsigned long long>(now.lost - last.lost),
                     static_cast<unsigned long long>(now.late - last.late), ms(feed.latency));
        last_feed_ = feed;
    }

    /// The jitter buffer's delay and what it had to make up: lost frames
    /// brought back by FEC, the rest concealed.
    void report_sound() {
        const PlaybackStats heard = heard_.take_interval();
        const std::optional<AudioPlayerStats> sound = viewer_.sound_stats();
        if (!sound) return;
        const JitterBuffer::Stats& jitter = sound->jitter;
        const JitterBuffer::Stats& last = last_sound_.jitter;
        std::fprintf(g_out,
                     "        sound %3llu fps  delay %3.0f ms (target %3.0f)  lost %llu  late %llu"
                     "  fec %4.0f ms  concealed %4.0f ms  breaks %llu",
                     static_cast<unsigned long long>(jitter.played - last.played), ms(sound->delay),
                     ms(sound->target), static_cast<unsigned long long>(jitter.lost - last.lost),
                     static_cast<unsigned long long>(jitter.late - last.late),
                     ms(sound->decoded.recovered - last_sound_.decoded.recovered),
                     ms(sound->decoded.concealed - last_sound_.decoded.concealed),
                     static_cast<unsigned long long>(heard.stalls));
        print_latency("latency", heard.latency);
        const PlayoutClockStats sync = viewer_.sync_stats();
        if (sync.apart.count() > last_apart_) {
            // Since the start: the clock keeps no intervals.
            std::fprintf(g_out, "  apart %4.1f ms (p95 %4.1f, mean %+5.1f)  floor %3.0f ms",
                         ms(sync.apart.percentile(0.5)), ms(sync.apart.percentile(0.95)),
                         ms(sync.mean_apart), ms(sync.floor));
            last_apart_ = sync.apart.count();
        }
        std::fprintf(g_out, "\n");
        last_sound_ = *sound;
    }

    void summarise_feed() {
        const DataPlayerStats feed = viewer_.feed_stats().value_or(last_feed_);
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
        const auto stats = viewer_.stats();
        if (!stats && last_stream_.bytes == 0) return;  // no pictures offered
        const VideoPlayerStats player = stats ? stats->first : last_player_;
        const IncomingStreamStats stream = stats ? stats->second : last_stream_;
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
        const std::optional<AudioPlayerStats> stats = viewer_.sound_stats();
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
        const PlayoutClockStats sync = viewer_.sync_stats();
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

    Viewer& viewer_;
    PlaybackMeter& decoded_;
    PlaybackMeter* const shown_;
    PlaybackMeter& heard_;
    CpuMeter cpu_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    VideoPlayerStats last_player_;
    IncomingStreamStats last_stream_;
    AudioPlayerStats last_sound_;
    DataPlayerStats last_feed_;
    uint64_t last_apart_ = 0;
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
    Viewer viewer({
        .sender = o.peer,
        .video = {.max_delay = o.buffer},
        .pictures =
            [&](VideoFrame picture) {
                decoded.picture(latency_of(picture));
#ifdef RATCAST_TOOLS_WINDOW
                if (window) window->show(std::move(picture));
#endif
            },
        .sound = output.get(),
        .heard = [&](Timestamp capture_time,
                     Timestamp play_time) { heard.picture(latency_of(capture_time, play_time)); },
        .sync = o.sync,
        .feed = feed.get(),
        .feed_config = {.latency = o.buffer},
    });

    // A viewer found by peer id listens too, so that the sender can dial back.
    librats::Node node(node_config(0, o.peer.has_value(), o.data_dir));
    auto* media = node.add_subsystem(std::make_unique<MediaService>(viewer.handlers()));
    viewer.attach(media->session());
    if (o.peer) add_discovery(node, discovery_key(*o.peer), o.data_dir);
    if (!node.start()) {
        std::cerr << "ratcast-recv: the node did not start\n";
        return 1;
    }
    decoded.start();
    if (shown) shown->start();
    heard.start();
    if (o.connect) {
        node.connect(o.connect->first, o.connect->second);
        std::fprintf(g_out, "connecting to %s:%u\n", o.connect->first.c_str(), o.connect->second);
    } else {
        std::fprintf(g_out, "looking for %s through the DHT (can take a minute)\n",
                     o.peer->short_hex().c_str());
    }

    Reporter reporter(viewer, decoded, shown ? &*shown : nullptr, heard);
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
    viewer.stop();
    node.stop();
    return played ? 0 : 1;
}
