#include "common/tool.h"

#include <atomic>
#include <charconv>
#include <csignal>
#include <memory>

#include "ratcast/engine/video_codec.h"

#include "librats/subsystems/dht_discovery.h"
#include "librats/util/logger.h"

// After librats: on Windows its headers bring in winsock2.h, which must come
// before windows.h.
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace ratcast::tools {
namespace {

std::atomic<bool> g_interrupted{false};

extern "C" void on_interrupt(int) { g_interrupted.store(true); }

std::optional<uint32_t> parse_number(std::string_view text) {
    uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

}  // namespace

librats::NodeConfig node_config(uint16_t port, bool listen, const std::string& data_dir) {
    librats::NodeConfig config;
    config.listen_port = port;
    config.enable_listen = listen;
    config.protocol = kProtocol;
    config.data_dir = data_dir;
    // Paths are sampled every 100 ms, as MediaService's rate control asks.
    return config;
}

void add_discovery(librats::Node& node, const std::string& key, const std::string& data_dir) {
    librats::DhtDiscovery::Config config;
    config.discovery_key = key;
    config.data_dir = data_dir;
    config.search_interval = std::chrono::seconds(10);
    node.add_subsystem(std::make_unique<librats::DhtDiscovery>(std::move(config)));
}

std::string discovery_key(const PeerId& sender) { return "ratcast/stream/" + sender.to_hex(); }

std::optional<std::pair<std::string, uint16_t>> parse_endpoint(std::string_view text) {
    std::string_view host;
    std::string_view port;
    if (text.starts_with('[')) {  // [ipv6]:port
        const size_t close = text.find("]:");
        if (close == std::string_view::npos) return std::nullopt;
        host = text.substr(1, close - 1);
        port = text.substr(close + 2);
    } else {
        const size_t colon = text.rfind(':');
        if (colon == std::string_view::npos || text.find(':') != colon) return std::nullopt;
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
    }
    const std::optional<uint32_t> number = parse_number(port);
    if (host.empty() || !number || *number == 0 || *number > 65535) return std::nullopt;
    return std::pair{std::string(host), static_cast<uint16_t>(*number)};
}

std::optional<Codec> parse_codec(std::string_view name) {
    if (name == "av1") return Codec::AV1;
    if (name == "h265" || name == "hevc") return Codec::H265;
    if (name == "h264") return Codec::H264;
    return std::nullopt;
}

std::optional<std::pair<uint32_t, uint32_t>> parse_size(std::string_view text) {
    const size_t x = text.find('x');
    if (x == std::string_view::npos) return std::nullopt;
    const std::optional<uint32_t> width = parse_number(text.substr(0, x));
    const std::optional<uint32_t> height = parse_number(text.substr(x + 1));
    if (!width || !height || *width == 0 || *height == 0) return std::nullopt;
    return std::pair{*width, *height};
}

void catch_interrupts() {
    std::signal(SIGINT, on_interrupt);
    std::signal(SIGTERM, on_interrupt);
}

bool interrupted() noexcept { return g_interrupted.load(); }

void set_verbose(bool verbose) {
    librats::Logger::getInstance().set_log_level(verbose ? librats::LogLevel::DEBUG
                                                         : librats::LogLevel::WARN);
    set_codec_logging(verbose);
}

Timestamp WallClock::now() const noexcept {
    return Timestamp{
        std::chrono::duration_cast<Duration>(std::chrono::system_clock::now().time_since_epoch())};
}

WallClock& WallClock::instance() noexcept {
    static WallClock clock;
    return clock;
}

Duration process_cpu_time() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return {};
    const auto ticks = [](FILETIME t) {  // 100 ns each
        return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return Duration{static_cast<Duration::rep>((ticks(kernel) + ticks(user)) / 10)};
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return {};
    const auto micros = [](timeval t) { return Duration{t.tv_sec * 1'000'000 + t.tv_usec}; };
    return micros(usage.ru_utime) + micros(usage.ru_stime);
#endif
}

CpuMeter::CpuMeter()
    : start_(Wall::now()), last_(start_), start_cpu_(process_cpu_time()), last_cpu_(start_cpu_) {}

double CpuMeter::take_interval() {
    const Wall::time_point now = Wall::now();
    const Duration cpu = process_cpu_time();
    const double result = percent(cpu - last_cpu_, now - last_);
    last_ = now;
    last_cpu_ = cpu;
    return result;
}

double CpuMeter::total() const {
    return percent(process_cpu_time() - start_cpu_, Wall::now() - start_);
}

double CpuMeter::percent(Duration cpu, Wall::duration wall) {
    const auto wall_us = std::chrono::duration_cast<Duration>(wall).count();
    return wall_us > 0 ? 100.0 * double(cpu.count()) / double(wall_us) : 0;
}

}  // namespace ratcast::tools
