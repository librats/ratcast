// ratcast-server: a public node calls behind NATs meet through and are
// carried by where nothing else gets through (ratcast/server/server.h).
//
//   ratcast-server [--port 7700] [--bind ADDRESS] [--data-dir DIR]
//                  [--relay-mbps 20] [--max-relayed 64] [--stats-every 10]
//                  [--verbose]

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "ratcast/server/server.h"

#include "librats/util/logger.h"

namespace {

constexpr const char* kUsage =
    "usage: ratcast-server [options]\n"
    "  --port N          the port calls dial, UDP and TCP (7700)\n"
    "  --bind ADDRESS    the interface to listen on (every one)\n"
    "  --data-dir DIR    where the server keeps its identity across runs\n"
    "  --relay-mbps N    what one carried connection may move each way (20)\n"
    "  --max-relayed N   connections carried at once (64)\n"
    "  --stats-every S   print what it does every S seconds, 0: never (10)\n"
    "  --verbose         librats' logs, all of them\n"
    "Calls reach it as rats://HOST:PORT (CallConfig::servers, the app's settings,\n"
    "the tools' --server).\n";

std::atomic<bool> g_interrupted{false};

extern "C" void on_interrupt(int) { g_interrupted.store(true); }

std::optional<uint32_t> number(std::string_view text, uint32_t max) {
    uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value > max)
        return std::nullopt;
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    ratcast::ServerConfig config;
    uint32_t stats_every = 10;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (name == "--help" || name == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        }
        if (name == "--verbose") {
            verbose = true;
            continue;
        }
        if (i + 1 == argc) {
            std::fprintf(stderr, "ratcast-server: %s: what? (--help)\n", argv[i]);
            return 2;
        }
        const std::string_view value = argv[++i];
        std::optional<uint32_t> n;
        if (name == "--bind") {
            config.bind_address = value;
        } else if (name == "--data-dir") {
            config.data_dir = value;
        } else if (name == "--port" && (n = number(value, 65535))) {
            config.port = static_cast<uint16_t>(*n);
        } else if (name == "--relay-mbps" && (n = number(value, 10000)) && *n > 0) {
            config.relay_rate_bps = uint64_t{*n} * 1'000'000;
        } else if (name == "--max-relayed" && (n = number(value, 100000))) {
            config.max_relayed = *n;
        } else if (name == "--stats-every" && (n = number(value, 86400))) {
            stats_every = *n;
        } else {
            std::fprintf(stderr, "ratcast-server: %s %s: what? (--help)\n", argv[i - 1], argv[i]);
            return 2;
        }
    }

    librats::Logger::getInstance().set_log_level(verbose ? librats::LogLevel::DEBUG
                                                         : librats::LogLevel::WARN);
    std::signal(SIGINT, on_interrupt);
    std::signal(SIGTERM, on_interrupt);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    auto started = ratcast::Server::start(config);
    if (!started) {
        std::fprintf(stderr, "ratcast-server: cannot listen on port %u (%s)\n",
                     unsigned{config.port}, ratcast::to_string(started.error()));
        return 1;
    }
    const std::unique_ptr<ratcast::Server>& server = *started;
    std::printf("ratcast-server %s on port %u\n", server->id().to_hex().c_str(),
                unsigned{server->port()});
    std::fflush(stdout);

    auto next = std::chrono::steady_clock::now();
    while (!g_interrupted.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (stats_every == 0 || std::chrono::steady_clock::now() < next) continue;
        next += std::chrono::seconds(stats_every);
        const ratcast::ServerStats stats = server->stats();
        std::printf(
            "peers %zu, registered %zu, relaying %zu (%.1f MB so far), punches %llu, "
            "forwarding %zu to %zu (%.1f MB)\n",
            stats.peers, stats.registrations, stats.relayed,
            static_cast<double>(stats.relayed_bytes) / 1e6,
            static_cast<unsigned long long>(stats.punches), stats.forwarded, stats.forwarded_to,
            static_cast<double>(stats.forwarded_bytes) / 1e6);
        std::fflush(stdout);
    }
    return 0;
}
