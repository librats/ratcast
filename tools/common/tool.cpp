#include "common/tool.h"

#include <atomic>
#include <charconv>
#include <csignal>
#include <memory>

#include "ratcast/engine/video_codec.h"

#include "librats/subsystems/dht_discovery.h"
#include "librats/util/logger.h"

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
    // Report how each viewer's path is doing, for the sender's stats.
    config.path_sample_ms = 1000;
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

}  // namespace ratcast::tools
