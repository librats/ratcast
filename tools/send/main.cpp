// ratcast-send: stream a screen, a window, a file or the test pattern to
// whoever connects. Prints how to connect, then a line of stats a second.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "common/arguments.h"
#include "common/tool.h"
#include "ratcast/client/video_publisher.h"
#include "ratcast/engine/file_source.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/screen_capture.h"
#include "ratcast/session/media_service.h"

using namespace ratcast;
using namespace ratcast::tools;
using namespace std::chrono_literals;

namespace {

constexpr const char* kUsage = R"(usage: ratcast-send [options]

What to send:
  --source screen|window|file|pattern   default: screen (the desktop asks which)
  --file PATH          the file for --source file (implied by --file)
  --once               play the file once instead of looping
  --size WxH           the pattern's size (default 1280x720)

How:
  --codec av1|h265|h264   default: av1
  --encoder NAME       a particular FFmpeg encoder (default: the best there is)
  --software           no hardware encoders
  --bitrate KBPS       default: 4000
  --fps N              frames a second at most (default 60; a file plays at its own)
  --latency MS         how long a frame is worth delivering (default 200)

Network:
  --port N             listen port (default 7700; 0 for any)
  --announce           be findable by peer id through the DHT (ratcast-recv --peer)
  --data-dir DIR       keep the peer id, and the screen choice, across runs
  --verbose            library logs
)";

struct Options {
    std::string source = "screen";
    std::string file;
    bool loop = true;
    uint32_t width = 1280;
    uint32_t height = 720;
    VideoEncoderConfig encoder;
    Duration latency = 200ms;
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
    o.source = args.text("source").value_or(o.file.empty() ? "screen" : "file");
    o.loop = !args.flag("once");
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
    o.encoder.frame_rate_num = args.number("fps", 1, 240).value_or(60);
    o.latency = std::chrono::milliseconds(args.number("latency", 10, 30'000).value_or(200));
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
        return std::make_unique<PatternSource>(PatternConfig{
            PixelFormat::NV12, o.width & ~1u, o.height & ~1u, o.encoder.frame_rate_num, 1});
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

/// One line a second: what went out, and how each viewer's path is doing.
class Reporter {
public:
    Reporter(librats::Node& node, MediaSession& session, VideoPublisher& publisher)
        : node_(node), session_(session), publisher_(publisher) {}

    void report() {
        const VideoPublisherStats now = publisher_.stats();
        const std::optional<OutgoingStreamStats> stream = session_.outgoing_stats(1);
        const uint64_t frames = now.frames - last_.frames;
        const double mbps = double(now.bytes - last_.bytes) * 8 / 1e6;
        const double encode_ms =
            frames > 0 ? double((now.encode_time - last_.encode_time).count()) / 1000 / frames : 0;
        std::printf(
            "[%4us] %3llu fps %6.2f Mbit/s  %ux%u %s  encode %5.2f ms  viewers %zu"
            "  skipped %llu  repeated %llu  drops %llu\n",
            seconds_++, static_cast<unsigned long long>(frames), mbps, now.width, now.height,
            now.encoder.c_str(), encode_ms, stream ? stream->subscribers : 0,
            static_cast<unsigned long long>(now.skipped - last_.skipped),
            static_cast<unsigned long long>(now.repeated - last_.repeated),
            static_cast<unsigned long long>(stream ? stream->backpressure_drops - last_drops_ : 0));
        for (const librats::PeerInfo& peer : node_.peers()) {
            const std::optional<librats::PathStats> path = node_.path_stats(peer.id);
            if (!path || !path->valid) continue;
            std::printf("        %s  rtt %5.1f ms  path %7.1f Mbit/s  loss %4.1f%%  expired %llu\n",
                        peer.id.short_hex().c_str(), path->srtt_us / 1000.0,
                        double(path->bandwidth_bps) / 1e6, path->loss * 100,
                        static_cast<unsigned long long>(path->live_expired));
        }
        std::fflush(stdout);
        last_ = now;
        last_drops_ = stream ? stream->backpressure_drops : 0;
    }

private:
    librats::Node& node_;
    MediaSession& session_;
    VideoPublisher& publisher_;
    VideoPublisherStats last_;
    uint64_t last_drops_ = 0;
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

    std::unique_ptr<VideoSource> source = open_source(o);
    if (!source) return 1;

    librats::Node node(node_config(o.port, true, o.data_dir));
    std::atomic<VideoPublisher*> publisher{nullptr};
    auto* media = node.add_subsystem(std::make_unique<MediaService>(SessionHandlers{
        .keyframe_needed =
            [&publisher](StreamId) {
                if (VideoPublisher* p = publisher.load()) p->request_keyframe();
            },
    }));
    if (o.announce) add_discovery(node, discovery_key(node.local_id()), o.data_dir);

    VideoPublisherConfig config;
    config.label = o.source;
    config.lifetime = o.latency;
    config.encoder = o.encoder;
    VideoPublisher video(media->session(), config);
    publisher = &video;

    if (!node.start()) {
        std::cerr << "ratcast-send: cannot listen on port " << o.port << "\n";
        return 1;
    }
    std::cout << "peer " << node.local_id().to_hex() << "\n"
              << "viewers connect with: ratcast-recv --connect <this host>:" << node.listen_port()
              << (o.announce
                      ? "\n                   or: ratcast-recv --peer " + node.local_id().to_hex()
                      : std::string())
              << "\n";

    std::atomic<bool> ended{false};
    std::atomic<bool> failed{false};
    if (Result<void> started = video.start(*source,
                                           [&](Result<void> outcome) {
                                               failed = !outcome;
                                               ended = true;
                                           });
        !started) {
        std::cerr << "ratcast-send: the source did not start: " << to_string(started.error())
                  << "\n";
        return 1;
    }

    Reporter reporter(node, media->session(), video);
    auto next = std::chrono::steady_clock::now() + 1s;
    while (!interrupted() && !ended) {
        std::this_thread::sleep_for(50ms);
        if (std::chrono::steady_clock::now() >= next) {
            next += 1s;
            reporter.report();
        }
    }
    if (ended) std::cout << (failed ? "the stream failed\n" : "the source ended\n");

    video.stop();
    publisher = nullptr;
    node.stop();
    return failed ? 1 : 0;
}
