// ratcast-recv: connect to a sender, play its video in a window (or without
// one), and print a line of stats a second and, on exit, a summary line.

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
#include "ratcast/client/playback_meter.h"
#include "ratcast/client/video_player.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/session/media_service.h"

#ifdef RATCAST_TOOLS_WINDOW
#include "recv/video_window.h"
#endif

using namespace ratcast;
using namespace ratcast::tools;
using namespace std::chrono_literals;

namespace {

constexpr const char* kUsage = R"(usage: ratcast-recv (--connect HOST:PORT | --peer ID) [options]

  --connect HOST:PORT  the sender's address
  --peer ID            the sender's peer id, found through the DHT (it runs --announce)
  --headless           decode, but show nothing
  --frames N           exit once N pictures are decoded
  --duration S         exit after S seconds
  --data-dir DIR       keep the peer id across runs
  --verbose            library logs

The pattern source's pictures carry the time of day they were captured: on
the sender's own host, or one whose clock is synchronised with it (NTP), the
latency from capture to decoded (and to presented) picture is printed too.
On exit a summary line, key=value, sums up the run.
)";

struct Options {
    std::optional<std::pair<std::string, uint16_t>> connect;
    std::optional<PeerId> peer;
    bool headless = false;
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
    o.frames = args.number("frames", 1, 1'000'000'000).value_or(0);
    o.duration = args.number("duration", 1, 1'000'000).value_or(0);
    o.data_dir = args.text("data-dir").value_or("");
    o.verbose = args.flag("verbose");
    for (const std::string& e : args.errors()) {
        std::cerr << "ratcast-recv: " << e << "\n";
        ok = false;
    }
    if (ok && !o.connect && !o.peer) {
        std::cerr << "ratcast-recv: say where the sender is: --connect or --peer\n";
        ok = false;
    }
    return ok ? std::optional(o) : std::nullopt;
}

/// Plays the first video stream the sender offers, and the next one when it
/// goes and comes back.
class Viewer {
public:
    Viewer(std::optional<PeerId> sender, VideoPlayer::Sink sink)
        : sender_(std::move(sender)), sink_(std::move(sink)) {}

    SessionHandlers handlers() {
        return SessionHandlers{
            .stream_announced = [this](const PeerId& peer,
                                       const StreamDescription& d) { announced(peer, d); },
            .stream_withdrawn = [this](const PeerId& peer, StreamId s) { withdrawn(peer, s); },
            .frame_received =
                [this](const PeerId& peer, EncodedFrame frame) {
                    std::lock_guard lock(mutex_);
                    if (playing(peer, frame.info.stream)) player_->push(std::move(frame));
                },
        };
    }

    void attach(MediaSession& session) { session_ = &session; }

    void stop() {
        std::lock_guard lock(mutex_);
        player_.reset();
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

private:
    bool playing(const PeerId& peer, StreamId stream) const {
        return player_ && player_->peer() == peer && player_->stream() == stream;
    }

    void announced(const PeerId& peer, const StreamDescription& description) {
        std::lock_guard lock(mutex_);
        if (playing(peer, description.id)) {
            player_->update(description);
            return;
        }
        if (player_ || description.kind() != MediaKind::Video) return;
        if (sender_ && peer != *sender_) return;

        auto player = std::make_unique<VideoPlayer>(*session_, peer, description, sink_);
        if (Result<void> started = player->start(); !started) {
            std::cerr << "cannot play stream '" << description.label
                      << "': " << to_string(started.error()) << "\n";
            return;
        }
        const VideoFormat* format = description.video();
        std::printf("playing '%s' from %s: %ux%u\n", description.label.c_str(),
                    peer.short_hex().c_str(), format->width, format->height);
        player_ = std::move(player);
    }

    void withdrawn(const PeerId& peer, StreamId stream) {
        std::lock_guard lock(mutex_);
        if (!playing(peer, stream)) return;
        std::printf("the stream ended\n");
        player_.reset();
    }

    const std::optional<PeerId> sender_;
    const VideoPlayer::Sink sink_;
    MediaSession* session_ = nullptr;
    std::mutex mutex_;
    std::unique_ptr<VideoPlayer> player_;
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

double ms(Duration d) { return double(d.count()) / 1000; }

/// One line a second, and a summary at the end.
class Reporter {
public:
    Reporter(Viewer& viewer, PlaybackMeter& decoded, PlaybackMeter* shown)
        : viewer_(viewer), decoded_(decoded), shown_(shown) {}

    void report() {
        const PlaybackStats decoded = decoded_.take_interval();
        const std::optional<PlaybackStats> shown =
            shown_ ? std::optional(shown_->take_interval()) : std::nullopt;
        const double cpu = cpu_.take_interval();
        const auto stats = viewer_.stats();
        if (!stats) {
            std::printf("[%4us] waiting for a stream  cpu %3.0f%%\n", seconds_++, cpu);
            std::fflush(stdout);
            return;
        }
        const auto& [player, stream] = *stats;
        const uint64_t pictures = player.pictures - last_player_.pictures;
        const double mbps = double(stream.bytes - last_stream_.bytes) * 8 / 1e6;
        const double decode_ms =
            pictures > 0 ? ms(player.decode_time - last_player_.decode_time) / double(pictures) : 0;
        std::printf(
            "[%4us] %3llu fps %6.2f Mbit/s  %ux%u %s  decode %5.2f ms  lost %llu  late %llu"
            "  dropped %llu  stalls %llu  cpu %3.0f%%",
            seconds_++, static_cast<unsigned long long>(pictures), mbps, player.width,
            player.height, player.decoder.c_str(), decode_ms,
            static_cast<unsigned long long>(stream.frames.lost - last_stream_.frames.lost),
            static_cast<unsigned long long>(stream.frames.late - last_stream_.frames.late),
            static_cast<unsigned long long>(dropped(player, stream) -
                                            dropped(last_player_, last_stream_)),
            static_cast<unsigned long long>(decoded.stalls), cpu);
        print_latency("latency", decoded.latency);
        if (shown) print_latency("shown", shown->latency);
        std::printf("\n");
        std::fflush(stdout);
        last_player_ = player;
        last_stream_ = stream;
    }

    /// The whole run as key=value pairs, for scripts. Call before the viewer stops.
    void summarise() {
        const PlaybackStats decoded = decoded_.total();
        const auto stats = viewer_.stats();
        const VideoPlayerStats player = stats ? stats->first : last_player_;
        const IncomingStreamStats stream = stats ? stats->second : last_stream_;
        const double first = decoded.first_picture ? ms(*decoded.first_picture) : -1;
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started_;
        // From the first picture on: what the viewer was shown, per second.
        const double playing = elapsed.count() - std::max(first, 0.0) / 1000;
        std::printf(
            "summary first_picture_ms=%.0f pictures=%llu stalls=%llu stalled_ms=%.0f"
            " longest_gap_ms=%.0f lost=%llu late=%llu dropped=%llu keyframe_requests=%llu"
            " fps=%.1f mbps=%.2f cpu=%.0f",
            first, static_cast<unsigned long long>(decoded.pictures),
            static_cast<unsigned long long>(decoded.stalls), ms(decoded.stalled),
            ms(decoded.longest_gap), static_cast<unsigned long long>(stream.frames.lost),
            static_cast<unsigned long long>(stream.frames.late),
            static_cast<unsigned long long>(dropped(player, stream)),
            static_cast<unsigned long long>(stream.frames.keyframe_requests +
                                            player.keyframe_requests),
            playing > 0 ? double(decoded.pictures) / playing : 0,
            double(stream.bytes) * 8 / 1e6 / std::max(elapsed.count(), 1.0), cpu_.total());
        print_percentiles("latency", decoded.latency);
        if (shown_) print_percentiles("shown", shown_->total().latency);
        std::printf("\n");
        std::fflush(stdout);
    }

private:
    /// Frames that reached this host and were never shown.
    static uint64_t dropped(const VideoPlayerStats& player, const IncomingStreamStats& stream) {
        return stream.frames.undecodable + player.dropped;
    }

    static void print_latency(const char* name, const LatencyHistogram& latency) {
        if (latency.count() == 0) return;
        std::printf("  %s %5.1f ms (p95 %5.1f, worst %5.1f)", name, ms(latency.mean()),
                    ms(latency.percentile(0.95)), ms(latency.max()));
    }

    static void print_percentiles(const char* name, const LatencyHistogram& latency) {
        if (latency.count() == 0) return;
        std::printf(" %s_mean_ms=%.1f %s_p50_ms=%.1f %s_p95_ms=%.1f %s_p99_ms=%.1f %s_max_ms=%.1f",
                    name, ms(latency.mean()), name, ms(latency.percentile(0.5)), name,
                    ms(latency.percentile(0.95)), name, ms(latency.percentile(0.99)), name,
                    ms(latency.max()));
    }

    Viewer& viewer_;
    PlaybackMeter& decoded_;
    PlaybackMeter* const shown_;
    CpuMeter cpu_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    VideoPlayerStats last_player_;
    IncomingStreamStats last_stream_;
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

#ifdef RATCAST_TOOLS_WINDOW
    std::unique_ptr<VideoWindow> window;
    if (!o.headless) {
        window = VideoWindow::open("ratcast");
        if (!window) return 1;
    }
#else
    if (!o.headless) std::cout << "built without SDL3: no window, as with --headless\n";
#endif

    // Pictures as decoded and, in a window, as presented. The wait for the
    // first one counts from the moment we dial.
    PlaybackMeter decoded;
    std::optional<PlaybackMeter> shown;
#ifdef RATCAST_TOOLS_WINDOW
    if (window) shown.emplace();
#endif
    Viewer viewer(o.peer, [&](VideoFrame picture) {
        decoded.picture(latency_of(picture));
#ifdef RATCAST_TOOLS_WINDOW
        if (window) window->show(std::move(picture));
#endif
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
    if (o.connect) {
        node.connect(o.connect->first, o.connect->second);
        std::printf("connecting to %s:%u\n", o.connect->first.c_str(), o.connect->second);
    } else {
        std::printf("looking for %s through the DHT (can take a minute)\n",
                    o.peer->short_hex().c_str());
    }

    Reporter reporter(viewer, decoded, shown ? &*shown : nullptr);
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
    const bool played = decoded.total().pictures > 0;
    viewer.stop();
    node.stop();
    return played ? 0 : 1;
}
