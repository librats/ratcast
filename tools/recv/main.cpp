// ratcast-recv: connect to a sender, play its video in a window (or without
// one), and print a line of stats a second.

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

The pattern source's pictures carry their capture time: on the sender's own
host, their latency from capture to decoded picture is printed too.
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

/// Pictures counted, and the latency of those that carry their capture time.
class Pictures {
public:
    void add(const VideoFrame& picture) {
        const std::optional<Timestamp> stamp = read_timestamp(picture);
        const Duration latency = stamp ? SteadyClock::instance().now() - *stamp : Duration{};
        std::lock_guard lock(mutex_);
        ++count_;
        // A stamp from another host's clock, or no stamp at all, says nothing.
        if (stamp && latency >= Duration::zero() && latency < 10s) {
            latency_sum_ += latency;
            latency_max_ = std::max(latency_max_, latency);
            ++latencies_;
        }
    }

    uint64_t count() {
        std::lock_guard lock(mutex_);
        return count_;
    }

    /// Average and worst latency since the last call; nullopt if none was known.
    std::optional<std::pair<double, double>> take_latency_ms() {
        std::lock_guard lock(mutex_);
        if (latencies_ == 0) return std::nullopt;
        const std::pair result{double(latency_sum_.count()) / 1000 / double(latencies_),
                               double(latency_max_.count()) / 1000};
        latency_sum_ = latency_max_ = Duration{};
        latencies_ = 0;
        return result;
    }

private:
    std::mutex mutex_;
    uint64_t count_ = 0;
    Duration latency_sum_{};
    Duration latency_max_{};
    uint64_t latencies_ = 0;
};

class Reporter {
public:
    Reporter(Viewer& viewer, Pictures& pictures) : viewer_(viewer), pictures_(pictures) {}

    void report() {
        const auto stats = viewer_.stats();
        if (!stats) {
            std::printf("[%4us] waiting for a stream\n", seconds_++);
            std::fflush(stdout);
            return;
        }
        const auto& [player, stream] = *stats;
        const uint64_t pictures = player.pictures - last_player_.pictures;
        const double mbps = double(stream.bytes - last_stream_.bytes) * 8 / 1e6;
        const double decode_ms =
            pictures > 0
                ? double((player.decode_time - last_player_.decode_time).count()) / 1000 / pictures
                : 0;
        std::printf(
            "[%4us] %3llu fps %6.2f Mbit/s  %ux%u %s  decode %5.2f ms  lost %llu  late %llu"
            "  dropped %llu",
            seconds_++, static_cast<unsigned long long>(pictures), mbps, player.width,
            player.height, player.decoder.c_str(), decode_ms,
            static_cast<unsigned long long>(stream.frames.lost - last_stream_.frames.lost),
            static_cast<unsigned long long>(stream.frames.late - last_stream_.frames.late),
            static_cast<unsigned long long>(
                (stream.frames.undecodable - last_stream_.frames.undecodable) +
                (player.dropped - last_player_.dropped)));
        if (const auto latency = pictures_.take_latency_ms()) {
            std::printf("  latency %5.1f ms (worst %5.1f)", latency->first, latency->second);
        }
        std::printf("\n");
        std::fflush(stdout);
        last_player_ = player;
        last_stream_ = stream;
    }

private:
    Viewer& viewer_;
    Pictures& pictures_;
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

    Pictures pictures;
    Viewer viewer(o.peer, [&](VideoFrame picture) {
        pictures.add(picture);
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
    if (o.connect) {
        node.connect(o.connect->first, o.connect->second);
        std::printf("connecting to %s:%u\n", o.connect->first.c_str(), o.connect->second);
    } else {
        std::printf("looking for %s through the DHT (can take a minute)\n",
                    o.peer->short_hex().c_str());
    }

    Reporter reporter(viewer, pictures);
    const auto start = std::chrono::steady_clock::now();
    auto next = start + 1s;
    const auto keep_going = [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            next += 1s;
            reporter.report();
        }
        if (o.frames > 0 && pictures.count() >= o.frames) return false;
        if (o.duration > 0 && now - start >= std::chrono::seconds(o.duration)) return false;
        return !interrupted();
    };
#ifdef RATCAST_TOOLS_WINDOW
    if (window) {
        window->run(keep_going);
    } else
#endif
    {
        while (keep_going()) std::this_thread::sleep_for(20ms);
    }

    viewer.stop();
    node.stop();
    const uint64_t decoded = pictures.count();
    std::printf("%llu pictures\n", static_cast<unsigned long long>(decoded));
    return decoded > 0 ? 0 : 1;
}
