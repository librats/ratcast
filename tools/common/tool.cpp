#include "common/tool.h"

#include <atomic>
#include <charconv>
#include <csignal>

#include "ratcast/engine/video_codec.h"

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
#ifdef SIGBREAK
    std::signal(SIGBREAK, on_interrupt);  // Windows: Ctrl-Break, what scripts can send
#endif
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);  // a reader of a feed out that went: a failed write
#endif
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

}  // namespace ratcast::tools
