// ratcast-send: stream a screen, a window, a file or the test pattern, and
// sound if asked — or an MPEG-TS feed as it is — to whoever connects. Prints how to connect, then a
// line of stats a second and, on exit, a summary line of key=value pairs.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "common/arguments.h"
#include "common/tool.h"
#include "ratcast/client/call.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/file_source.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/screen_capture.h"
#include "ratcast/engine/tone_source.h"
#include "ratcast/engine/ts_io.h"

using namespace ratcast;
using namespace ratcast::tools;
using namespace std::chrono_literals;

namespace {

constexpr StreamId kVideoStream = 1;
constexpr StreamId kFeedStream = 3;

constexpr const char* kUsage = R"(usage: ratcast-send [options]

What to send:
  --source screen|window|file|pattern   default: screen (the desktop asks which)
  --file PATH          the file for --source file (implied by --file)
  --once               play the file once instead of looping
  --size WxH           the pattern's size (default 1280x720)
  --input URL          an MPEG-TS feed instead, carried as it is, as librist and SRT
                       carry it: udp://[host]:port (a multicast group is joined), - for
                       standard input, or a .ts file (played in real time)

How:
  --codec av1|h265|h264   default: av1
  --encoder NAME       a particular FFmpeg encoder (default: the best there is)
  --software           no hardware encoders
  --bitrate KBPS       the most rate control goes up to (default 4000)
  --start-bitrate KBPS where rate control starts (default 2000)
  --fixed-bitrate      no rate control: always --bitrate
  --fps N              frames a second at most (default 60; a file plays at its own)
  --latency MS         how long a frame is worth delivering (default 200); for a feed,
                       what repair has to work in, as SRT's latency or librist's buffer
  --keyframe-every S   a keyframe every S seconds as well as when asked (default: only
                       when asked); bursts to measure what else waits behind them

Sound:
  --audio tone|mic|system   send sound too: a tone (stamped like the pattern), the
                       microphone, or what this computer plays
  --music              code it as music: stereo, full band, without DTX or FEC
                       (implied by --audio system)
  --audio-bitrate KBPS default 32, or 128 for music

Network:
  --port N             listen port (default 7700; 0 for any)
  --announce           be findable by peer id through the DHT (ratcast-recv --peer)
  --data-dir DIR       keep the peer id, and the screen choice, across runs
  --verbose            library logs
)";

struct Options {
    std::string source = "screen";
    std::string input;  ///< a feed instead of a source
    std::string file;
    bool loop = true;
    uint32_t width = 1280;
    uint32_t height = 720;
    VideoEncoderConfig encoder;
    bool rate_control = true;
    uint32_t start_bitrate = 2'000'000;
    Duration latency = 200ms;
    uint32_t keyframe_every = 0;  ///< seconds; 0: only when asked
    std::string audio;            ///< empty: no sound
    AudioEncoderConfig audio_encoder;
    uint16_t port = 7700;
    bool announce = false;
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
    o.file = args.text("file").value_or("");
    const std::optional<std::string> source = args.text("source");
    const bool args_gave_source = source || !o.file.empty();
    o.source = source.value_or(o.file.empty() ? "screen" : "file");
    o.loop = !args.flag("once");
    o.input = args.text("input").value_or("");
    if (const auto size = args.text("size")) {
        if (const auto parsed = parse_size(*size)) {
            std::tie(o.width, o.height) = *parsed;
        } else {
            std::cerr << "--size wants WxH, like 1280x720\n";
            return std::nullopt;
        }
    }
    if (const auto codec = args.text("codec")) {
        const std::optional<Codec> parsed = parse_codec(*codec);
        if (!parsed) {
            std::cerr << "--codec wants av1, h265 or h264\n";
            return std::nullopt;
        }
        o.encoder.codec = *parsed;
    }
    o.encoder.encoder = args.text("encoder").value_or("");
    o.encoder.allow_hardware = !args.flag("software");
    o.encoder.bitrate = args.number("bitrate", 50, 500'000).value_or(4000) * 1000;
    o.start_bitrate = args.number("start-bitrate", 50, 500'000).value_or(2000) * 1000;
    o.rate_control = !args.flag("fixed-bitrate");
    o.encoder.frame_rate_num = args.number("fps", 1, 240).value_or(60);
    o.latency = std::chrono::milliseconds(args.number("latency", 10, 30'000).value_or(200));
    o.keyframe_every = args.number("keyframe-every", 1, 3600).value_or(0);
    o.audio = args.text("audio").value_or("");
    const bool music = args.flag("music") || o.audio == "system";
    o.audio_encoder.content = music ? AudioContent::Music : AudioContent::Voice;
    o.audio_encoder.format = {48'000, music ? uint8_t{2} : uint8_t{1}};
    o.audio_encoder.bitrate =
        args.number("audio-bitrate", 6, 510).value_or(music ? 128 : 32) * 1000;
    // A voice: silence sent as nothing, and FEC for some loss. Music has
    // neither: Opus codes it in CELT, which carries no FEC.
    o.audio_encoder.dtx = !music;
    o.audio_encoder.expected_loss = music ? 0 : 10;
    o.port = static_cast<uint16_t>(args.number("port", 0, 65535).value_or(7700));
    o.announce = args.flag("announce");
    o.data_dir = args.text("data-dir").value_or("");
    o.verbose = args.flag("verbose");

    const std::vector<std::string> errors = args.errors();
    for (const std::string& e : errors) std::cerr << "ratcast-send: " << e << "\n";
    if (o.source != "screen" && o.source != "window" && o.source != "file" &&
        o.source != "pattern") {
        std::cerr << "ratcast-send: --source wants screen, window, file or pattern\n";
        return std::nullopt;
    }
    if (!o.input.empty() && (args_gave_source || !o.audio.empty())) {
        std::cerr << "ratcast-send: --input is a feed instead of a source and sound\n";
        return std::nullopt;
    }
    if (!o.audio.empty() && o.audio != "tone" && o.audio != "mic" && o.audio != "system") {
        std::cerr << "ratcast-send: --audio wants tone, mic or system\n";
        return std::nullopt;
    }
    if (o.source == "file" && o.file.empty()) {
        std::cerr << "ratcast-send: --source file needs --file PATH\n";
        return std::nullopt;
    }
    if (!errors.empty()) return std::nullopt;
    return o;
}

/// The restore token kept for `source` in the data directory, if any.
std::filesystem::path token_path(const Options& o) {
    return std::filesystem::path(o.data_dir) / (o.source + ".token");
}

std::unique_ptr<ScreenCapture> open_screen(const Options& o) {
    ScreenCaptureOptions options;
    options.monitors = o.source == "screen";
    options.windows = o.source == "window";
    options.max_frame_rate = o.encoder.frame_rate_num;
    if (!o.data_dir.empty()) std::getline(std::ifstream(token_path(o)), options.restore_token);

    // The desktop's picker may wait on the user; Ctrl-C calls it off.
    std::stop_source cancel;
    std::jthread watch([&cancel](std::stop_token done) {
        while (!done.stop_requested() && !interrupted()) std::this_thread::sleep_for(50ms);
        if (interrupted()) cancel.request_stop();
    });
    if (options.restore_token.empty()) std::cout << "choose what to share in the dialog...\n";
    Result<std::unique_ptr<ScreenCapture>> capture =
        open_screen_capture(options, cancel.get_token());
    if (!capture) {
        std::cerr << "ratcast-send: no screen capture: " << to_string(capture.error()) << "\n";
        return nullptr;
    }
    if (!o.data_dir.empty() && !(*capture)->restore_token().empty()) {
        std::filesystem::create_directories(o.data_dir);
        std::ofstream(token_path(o)) << (*capture)->restore_token() << "\n";
    }
    return std::move(*capture);
}

std::unique_ptr<VideoSource> open_source(Options& o) {
    if (o.source == "pattern") {
        // Stamped with the time of day, so a viewer on a synchronised host
        // can tell the latency as well as one on this host.
        return std::make_unique<PatternSource>(
            PatternConfig{PixelFormat::NV12, o.width & ~1u, o.height & ~1u,
                          o.encoder.frame_rate_num, 1},
            WallClock::instance());
    }
    if (o.source == "file") {
        Result<std::unique_ptr<FileSource>> file =
            open_file_source({o.file, PixelFormat::NV12, o.loop});
        if (!file) {
            std::cerr << "ratcast-send: cannot play " << o.file << ": " << to_string(file.error())
                      << "\n";
            return nullptr;
        }
        if ((*file)->info().frame_rate_num > 0) {
            o.encoder.frame_rate_num = (*file)->info().frame_rate_num;
            o.encoder.frame_rate_den = (*file)->info().frame_rate_den;
        }
        return std::move(*file);
    }
    return open_screen(o);
}

std::unique_ptr<AudioSource> open_audio(const Options& o) {
    if (o.audio == "tone") {
        // Stamped with the time of day, as the pattern is. Loud enough for
        // Opus to take it for speech, and code FEC for it.
        return std::make_unique<ToneSource>(
            ToneConfig{.format = o.audio_encoder.format, .level = 0.5f}, WallClock::instance());
    }
    Result<std::unique_ptr<AudioSource>> capture =
        open_audio_capture({.format = o.audio_encoder.format, .system_sound = o.audio == "system"});
    if (!capture) {
        std::cerr << "ratcast-send: no sound capture: " << to_string(capture.error()) << "\n";
        return nullptr;
    }
    return std::move(*capture);
}

/// What is published: pictures (with sound, perhaps), or a feed.
struct Published {
    VideoPublisher* video = nullptr;
    const AudioPublisher* sound = nullptr;
    const DataPublisher* feed = nullptr;
};

/// One line a second: what went out, and how each viewer's path is doing;
/// a summary at the end.
class Reporter {
public:
    Reporter(Call& call, Published published) : call_(call), published_(published) {}

    void report() {
        std::printf("[%4us]", seconds_++);
        if (published_.video) report_video();
        if (published_.feed) report_feed();
        std::printf("  cpu %3.0f%%\n", cpu_.take_interval());
        if (published_.sound) {
            const AudioPublisherStats sound = published_.sound->stats();
            std::printf("        sound %3llu fps %5.1f kbit/s  %s\n",
                        static_cast<unsigned long long>(sound.frames - last_sound_.frames),
                        double(sound.bytes - last_sound_.bytes) * 8 / 1e3, sound.encoder.c_str());
            last_sound_ = sound;
        }
        for (const PeerId& peer : call_.peers()) {
            const std::optional<librats::PathStats> path = call_.path(peer);
            if (!path || !path->valid) continue;
            librats::PathStats& last = paths_[peer];
            const uint64_t lost = path->bytes_lost - last.bytes_lost;
            const uint64_t resolved = lost + path->bytes_delivered - last.bytes_delivered;
            const double queue_ms = path->bandwidth_bps > 0 ? double(path->queued_bytes) * 8e3 /
                                                                  double(path->bandwidth_bps)
                                                            : 0;
            // "busy": the estimate was taken with the path kept busy, so it is
            // what the path carries rather than a floor.
            std::printf(
                "        %s  rtt %5.1f ms  path %7.1f Mbit/s%s  queue %4.0f ms  loss %4.1f%%"
                "  expired %llu  repair %5.2f Mbit/s\n",
                peer.short_hex().c_str(), path->srtt_us / 1000.0, double(path->bandwidth_bps) / 1e6,
                path->app_limited ? "     " : " busy", queue_ms,
                resolved > 0 ? 100.0 * double(lost) / double(resolved) : 0.0,
                static_cast<unsigned long long>(path->live_expired),
                double(path->live_repair_bytes - last.live_repair_bytes) * 8 / 1e6);
            last = *path;
        }
        std::fflush(stdout);
    }

    /// The whole run as key=value pairs, for scripts. Live messages are counted
    /// over every viewer, as last reported: call after a last report().
    void summarise() {
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started_;
        const double seconds = std::max(elapsed.count(), 1.0);
        std::printf("summary");
        if (published_.video) summarise_video(seconds);
        if (published_.feed) {
            const DataPublisherStats feed = published_.feed->stats();
            std::printf(" data_frames=%llu data_packets=%llu data_skipped=%llu data_mbps=%.2f",
                        static_cast<unsigned long long>(feed.frames),
                        static_cast<unsigned long long>(feed.packets),
                        static_cast<unsigned long long>(feed.skipped),
                        double(feed.packets * kTsPacketSize) * 8 / 1e6 / seconds);
        }
        if (published_.sound) {
            const AudioPublisherStats sound = published_.sound->stats();
            std::printf(" audio_frames=%llu audio_kbps=%.1f audio_dropped=%llu",
                        static_cast<unsigned long long>(sound.frames),
                        double(sound.bytes) * 8 / 1e3 / seconds,
                        static_cast<unsigned long long>(sound.dropped));
        }
        uint64_t live_sent = 0, live_expired = 0, repair = 0, delivered = 0;
        for (const auto& [peer, path] : paths_) {
            live_sent += path.live_sent;
            live_expired += path.live_expired;
            repair += path.live_repair_bytes;
            delivered += path.bytes_delivered;
        }
        std::printf(" live_sent=%llu live_expired=%llu repair_share=%.3f cpu=%.0f\n",
                    static_cast<unsigned long long>(live_sent),
                    static_cast<unsigned long long>(live_expired),
                    delivered > 0 ? double(repair) / double(delivered) : 0, cpu_.total());
        std::fflush(stdout);
    }

private:
    void report_video() {
        const VideoPublisherStats now = published_.video->stats();
        const std::optional<OutgoingStreamStats> stream =
            call_.session().outgoing_stats(kVideoStream);
        const uint64_t frames = now.frames - last_.frames;
        const double mbps = double(now.bytes - last_.bytes) * 8 / 1e6;
        const double encode_ms =
            frames > 0 ? double((now.encode_time - last_.encode_time).count()) / 1000 / frames : 0;
        std::printf(
            " %3llu fps %6.2f Mbit/s (aim %5.2f)  %ux%u %s  encode %5.2f ms  viewers %zu"
            "  skipped %llu  repeated %llu  keyframes %llu  drops %llu",
            static_cast<unsigned long long>(frames), mbps, now.bitrate / 1e6, now.width, now.height,
            now.encoder.c_str(), encode_ms, stream ? stream->subscribers : 0,
            static_cast<unsigned long long>(now.skipped - last_.skipped),
            static_cast<unsigned long long>(now.repeated - last_.repeated),
            static_cast<unsigned long long>(now.keyframes - last_.keyframes),
            static_cast<unsigned long long>(stream ? stream->backpressure_drops - last_drops_ : 0));
        last_ = now;
        last_drops_ = stream ? stream->backpressure_drops : 0;
    }

    void report_feed() {
        const DataPublisherStats now = published_.feed->stats();
        const std::optional<OutgoingStreamStats> stream =
            call_.session().outgoing_stats(kFeedStream);
        std::printf(
            " feed %6.2f Mbit/s  %4llu frames  viewers %zu  skipped %llu bytes  drops %llu",
            double((now.packets - last_feed_.packets) * kTsPacketSize) * 8 / 1e6,
            static_cast<unsigned long long>(now.frames - last_feed_.frames),
            stream ? stream->subscribers : 0,
            static_cast<unsigned long long>(now.skipped - last_feed_.skipped),
            static_cast<unsigned long long>(stream ? stream->backpressure_drops - last_drops_ : 0));
        last_feed_ = now;
        last_drops_ = stream ? stream->backpressure_drops : 0;
    }

    void summarise_video(double seconds) {
        const VideoPublisherStats now = published_.video->stats();
        const std::optional<OutgoingStreamStats> stream =
            call_.session().outgoing_stats(kVideoStream);
        std::printf(
            " frames=%llu keyframes=%llu keyframe_kb=%.0f keyframes_asked=%llu skipped=%llu"
            " encode_ms=%.2f mbps=%.2f bitrate_cuts=%llu bitrate_raises=%llu",
            static_cast<unsigned long long>(now.frames),
            static_cast<unsigned long long>(now.keyframes),
            now.keyframes > 0 ? double(now.keyframe_bytes) / 1024 / double(now.keyframes) : 0,
            static_cast<unsigned long long>(stream ? stream->keyframes_asked : 0),
            static_cast<unsigned long long>(now.skipped),
            now.frames > 0 ? double(now.encode_time.count()) / 1000 / double(now.frames) : 0,
            double(now.bytes) * 8 / 1e6 / seconds,
            static_cast<unsigned long long>(stream ? stream->bitrate_cuts : 0),
            static_cast<unsigned long long>(stream ? stream->bitrate_raises : 0));
    }

    Call& call_;
    const Published published_;
    CpuMeter cpu_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    VideoPublisherStats last_;
    DataPublisherStats last_feed_;
    AudioPublisherStats last_sound_;
    uint64_t last_drops_ = 0;
    /// The last word on each viewer's path, kept after it leaves.
    std::map<PeerId, librats::PathStats> paths_;
    unsigned seconds_ = 1;
};

}  // namespace

int main(int argc, char** argv) {
    std::optional<Options> options = parse(argc, argv);
    if (!options) {
        std::cerr << "\n" << kUsage;
        return 2;
    }
    Options& o = *options;
    set_verbose(o.verbose);
    catch_interrupts();

    // A feed, or a source with sound perhaps.
    std::unique_ptr<TsInput> input;
    std::unique_ptr<VideoSource> source;
    std::unique_ptr<AudioSource> audio_source;
    if (!o.input.empty()) {
        Result<std::unique_ptr<TsInput>> opened = open_ts_input(o.input);
        if (!opened) {
            std::cerr << "ratcast-send: cannot read " << o.input << ": "
                      << to_string(opened.error()) << "\n";
            return 1;
        }
        input = std::move(*opened);
    } else {
        if (!(source = open_source(o))) return 1;
        if (!o.audio.empty() && !(audio_source = open_audio(o))) return 1;
    }

    // Whichever ends first ends the run.
    std::atomic<bool> ended{false};
    std::atomic<bool> failed{false};
    Result<std::unique_ptr<Call>> started =
        Call::start({.port = o.port, .data_dir = o.data_dir, .discoverable = o.announce},
                    {.source_ended = [&](StreamId, Result<void> outcome) {
                        failed = !outcome;
                        ended = true;
                    }});
    if (!started) {
        std::cerr << "ratcast-send: cannot listen on port " << o.port << "\n";
        return 1;
    }
    Call& call = **started;

    const auto check = [](const auto& published, const char* what) {
        if (!published) {
            std::cerr << "ratcast-send: the " << what
                      << " did not start: " << to_string(published.error()) << "\n";
        }
        return published.has_value();
    };
    Published published;
    if (source) {
        VideoPublisherConfig config;
        config.stream = kVideoStream;
        config.label = o.source;
        config.lifetime = o.latency;
        config.encoder = o.encoder;
        config.rate_control = o.rate_control;
        config.start_bitrate = o.start_bitrate;
        const Result<VideoPublisher*> video = call.publish(std::move(source), config);
        if (!check(video, "source")) return 1;
        published.video = *video;
    }
    if (audio_source) {
        const Result<AudioPublisher*> sound =
            call.publish(std::move(audio_source),
                         AudioPublisherConfig{
                             .label = o.audio, .lifetime = o.latency, .encoder = o.audio_encoder});
        if (!check(sound, "sound")) return 1;
        published.sound = *sound;
    }
    if (input) {
        const Result<DataPublisher*> feed = call.publish(
            std::move(input), DataPublisherConfig{.stream = kFeedStream, .lifetime = o.latency});
        if (!check(feed, "feed")) return 1;
        published.feed = *feed;
    }

    std::cout << "peer " << call.id().to_hex() << "\n";
    std::vector<std::string> invites = call.invites();
    if (invites.empty())
        invites.push_back(to_string(RatsUrl{librats::HostEndpoint{"<this host>", call.port()}}));
    const char* lead = "viewers connect with: ";
    for (const std::string& invite : invites) {
        std::cout << lead << "ratcast-recv " << invite << "\n";
        lead = "                   or: ";
    }

    Reporter reporter(call, published);
    auto next = std::chrono::steady_clock::now() + 1s;
    const auto keyframe_every = std::chrono::seconds(o.keyframe_every);
    auto next_keyframe = std::chrono::steady_clock::now() + keyframe_every;
    while (!interrupted() && !ended) {
        std::this_thread::sleep_for(50ms);
        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            next += 1s;
            reporter.report();
        }
        if (published.video && o.keyframe_every > 0 && now >= next_keyframe) {
            next_keyframe += keyframe_every;
            published.video->request_keyframe();
        }
    }
    if (ended) std::cout << (failed ? "the stream failed\n" : "the source ended\n");
    reporter.report();
    reporter.summarise();

    started->reset();
    return failed ? 1 : 0;
}
