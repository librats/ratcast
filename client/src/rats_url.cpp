#include "ratcast/client/rats_url.h"

namespace ratcast {
namespace {

constexpr std::string_view kScheme = "rats://";
constexpr std::string_view kVia = "via=";

}  // namespace

std::optional<RatsUrl> parse_rats_url(std::string_view url) {
    if (!url.starts_with(kScheme)) return std::nullopt;
    url.remove_prefix(kScheme.size());
    if (url.ends_with('/')) url.remove_suffix(1);
    if (auto endpoint = librats::HostEndpoint::parse(url)) return RatsUrl{std::move(*endpoint)};
    if (auto peer = PeerId::from_hex(url)) return RatsUrl{*peer};
    return std::nullopt;
}

std::string to_string(const RatsUrl& url) {
    if (const auto* endpoint = std::get_if<librats::HostEndpoint>(&url)) {
        return std::string(kScheme) + endpoint->to_string();
    }
    return std::string(kScheme) + std::get<PeerId>(url).to_hex();
}

std::optional<RatsLink> parse_rats_link(std::string_view link) {
    const size_t query = link.find('?');
    std::optional<RatsUrl> url = parse_rats_url(link.substr(0, query));
    if (!url) return std::nullopt;
    RatsLink parsed{std::move(*url)};
    if (query == std::string_view::npos) return parsed;
    std::string_view rest = link.substr(query + 1);
    while (!rest.empty()) {
        const size_t amp = rest.find('&');
        const std::string_view parameter = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
        if (!parameter.starts_with(kVia)) continue;
        std::optional<librats::HostEndpoint> server = parse_server(parameter.substr(kVia.size()));
        if (!server) return std::nullopt;
        parsed.via.push_back(std::move(*server));
    }
    return parsed;
}

std::string to_string(const RatsLink& link) {
    std::string text = to_string(link.url);
    char separator = '?';
    for (const librats::HostEndpoint& server : link.via) {
        text += separator;
        text += kVia;
        text += server.to_string();
        separator = '&';
    }
    return text;
}

std::optional<librats::HostEndpoint> parse_server(std::string_view text) {
    if (text.starts_with(kScheme)) text.remove_prefix(kScheme.size());
    if (text.ends_with('/')) text.remove_suffix(1);
    if (auto endpoint = librats::HostEndpoint::parse(text)) return endpoint;
    // No port: a name, an IPv4 address, or an IPv6 one in brackets.
    if (text.empty() || text.find('/') != std::string_view::npos) return std::nullopt;
    if (text.front() == '[') {
        if (text.back() != ']' || text.size() < 3) return std::nullopt;
        text = text.substr(1, text.size() - 2);
    } else if (text.find(':') != std::string_view::npos) {
        return std::nullopt;
    }
    return librats::HostEndpoint(std::string(text), kServerPort);
}

}  // namespace ratcast
