#pragma once

/**
 * @file rats_url.h
 * @brief rats:// links: where a ratcast node is reached.
 *
 * Two kinds: "rats://HOST:PORT" (or "rats://[IPV6]:PORT"), an address to
 * dial, and "rats://PEER-ID", the peer id in hex of a node that is
 * discoverable, looked for through the DHT. A trailing slash is allowed.
 * A link (RatsLink) may name servers too: "rats://PEER-ID?via=HOST:PORT",
 * where the room is met that its members may not know of. And a server, as a
 * user writes one (parse_server()).
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ratcast/session/media_network.h"

#include "librats/core/host_endpoint.h"

namespace ratcast {

using RatsUrl = std::variant<librats::HostEndpoint, PeerId>;

/// Nullopt for anything that is neither kind.
std::optional<RatsUrl> parse_rats_url(std::string_view url);

std::string to_string(const RatsUrl& url);

/// Where to go, and the servers the room is met at that a member may not know
/// of: "?via=…", one or more joined by '&' (any other parameter passed over).
struct RatsLink {
    RatsUrl url;
    std::vector<librats::HostEndpoint> via = {};

    bool operator==(const RatsLink&) const = default;
};

/// A RatsUrl with servers after it, if any; nullopt for anything else, a via
/// that is no server too.
std::optional<RatsLink> parse_rats_link(std::string_view link);

std::string to_string(const RatsLink& link);

/// The port a ratcast-server listens on unless told otherwise.
inline constexpr uint16_t kServerPort = 7700;

/// A server as a user writes one: "HOST", "HOST:PORT" or "rats://HOST[:PORT]"
/// ("[IPV6]" for an IPv6 address), the port kServerPort if none is given.
/// Nullopt for anything else.
std::optional<librats::HostEndpoint> parse_server(std::string_view text);

}  // namespace ratcast
