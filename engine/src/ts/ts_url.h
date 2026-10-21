#pragma once

/**
 * @file ts_url.h
 * @brief Where a feed comes from or goes to, as ts_io.h spells it.
 */

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ratcast {

struct TsUrl {
    enum class Kind : uint8_t { Udp, Stdio, File };
    Kind kind = Kind::File;
    std::string host{};  ///< of udp://; empty for any
    uint16_t port = 0;
    std::string path{};  ///< of a file
};

/// `url` taken apart; nullopt for a udp:// without a port, or an empty one.
inline std::optional<TsUrl> parse_ts_url(std::string_view url) {
    if (url.empty()) return std::nullopt;
    if (url == "-") return TsUrl{.kind = TsUrl::Kind::Stdio};
    constexpr std::string_view kUdp = "udp://";
    if (!url.starts_with(kUdp)) return TsUrl{.kind = TsUrl::Kind::File, .path = std::string(url)};

    std::string_view rest = url.substr(kUdp.size());
    if (rest.starts_with('@')) rest.remove_prefix(1);  // ffmpeg's "listen here"
    rest = rest.substr(0, rest.find_first_of("?/"));   // options this reader has none of
    const size_t colon = rest.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    std::string_view host = rest.substr(0, colon);
    if (host.starts_with('[') && host.ends_with(']')) host = host.substr(1, host.size() - 2);
    const std::string_view port = rest.substr(colon + 1);
    uint16_t number = 0;
    const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
    if (error != std::errc{} || end != port.data() + port.size() || number == 0) {
        return std::nullopt;
    }
    return TsUrl{.kind = TsUrl::Kind::Udp, .host = std::string(host), .port = number};
}

}  // namespace ratcast
