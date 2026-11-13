#include "ratcast/client/rats_url.h"

#include <algorithm>
#include <cctype>

#include "bytes.h"
#include "ratcast/client/identity.h"

namespace ratcast {
namespace {

constexpr std::string_view kScheme = "rats://";
/// A room's secret goes in the path, after this: in the host's place a URL
/// parser lowercases it (Qt's, as Android's and macOS's links reach the app).
constexpr std::string_view kRoomPath = "r/";
constexpr std::string_view kVia = "via=";

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// %XX decoded; nullopt for a '%' that is no escape.
std::optional<std::string> percent_decoded(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            decoded += text[i];
            continue;
        }
        if (i + 2 >= text.size()) return std::nullopt;
        const int high = hex_digit(text[i + 1]);
        const int low = hex_digit(text[i + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        decoded += static_cast<char>(high * 16 + low);
        i += 2;
    }
    return decoded;
}

/// The rats:// link a web link carries — https (or http), the website (or its
/// www.), its page /r/ (or /r), the link after '#' — or nullopt for anything
/// that is no web link.
std::optional<std::string> from_web_link(std::string_view link) {
    const size_t scheme_end = link.find("://");
    if (scheme_end == std::string_view::npos) return std::nullopt;
    const std::string_view scheme = link.substr(0, scheme_end);
    if (!equal_ignoring_case(scheme, "https") && !equal_ignoring_case(scheme, "http"))
        return std::nullopt;
    std::string_view rest = link.substr(scheme_end + 3);
    const size_t hash = rest.find('#');
    if (hash == std::string_view::npos) return std::nullopt;
    const std::string_view before = rest.substr(0, hash);
    const size_t slash = before.find('/');
    const std::string_view host = before.substr(0, slash);
    const std::string_view path =
        slash == std::string_view::npos ? std::string_view() : before.substr(slash);
    if (!equal_ignoring_case(host, "ratcast.app") && !equal_ignoring_case(host, "www.ratcast.app"))
        return std::nullopt;
    if (path != "/r/" && path != "/r") return std::nullopt;
    std::optional<std::string> carried = percent_decoded(rest.substr(hash + 1));
    if (!carried || carried->empty()) return std::nullopt;
    return std::string(kScheme) + *carried;
}

}  // namespace

RoomSecret RoomSecret::random() { return RoomSecret{random_bytes<16>()}; }

std::string room_key(const RoomSecret& room) {
    constexpr std::string_view what = "ratcast room";
    return "ratcast/0/room/" + to_hex(mac(room.bytes, bytes_of(what)));
}

std::optional<RatsUrl> parse_rats_url(std::string_view url) {
    if (!url.starts_with(kScheme)) return std::nullopt;
    url.remove_prefix(kScheme.size());
    if (url.ends_with('/')) url.remove_suffix(1);
    if (url.starts_with(kRoomPath)) {
        url.remove_prefix(kRoomPath.size());  // a room's, nothing else
    } else {
        if (auto endpoint = librats::HostEndpoint::parse(url)) return RatsUrl{std::move(*endpoint)};
        if (auto peer = PeerId::from_hex(url)) return RatsUrl{*peer};
    }
    // Neither a port nor 64 hex digits: 22 of base64url (as written before
    // "r/", too).
    if (const std::optional<std::vector<uint8_t>> secret = from_base64url(url, 16)) {
        RoomSecret room;
        std::copy(secret->begin(), secret->end(), room.bytes.begin());
        return RatsUrl{room};
    }
    return std::nullopt;
}

std::string to_string(const RatsUrl& url) {
    if (const auto* endpoint = std::get_if<librats::HostEndpoint>(&url)) {
        return std::string(kScheme) + endpoint->to_string();
    }
    if (const auto* room = std::get_if<RoomSecret>(&url)) {
        return std::string(kScheme) + std::string(kRoomPath) + to_base64url(room->bytes);
    }
    return std::string(kScheme) + std::get<PeerId>(url).to_hex();
}

std::optional<RatsLink> parse_rats_link(std::string_view link) {
    if (!link.starts_with(kScheme)) {
        const std::optional<std::string> carried = from_web_link(link);
        if (!carried) return std::nullopt;
        return parse_rats_link(*carried);
    }
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

std::string to_web_link(const RatsLink& link) {
    // The page's own path stands for a room's: "/r/#SECRET", not "/r/#r/SECRET".
    const std::string text = to_string(link);
    const size_t carried =
        kScheme.size() + (std::holds_alternative<RoomSecret>(link.url) ? kRoomPath.size() : 0);
    return std::string(kWebLinkPrefix) + text.substr(carried);
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
