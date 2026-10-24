#pragma once

/**
 * @file rats_url.h
 * @brief rats:// links: where a ratcast node is reached.
 *
 * Two kinds: "rats://HOST:PORT" (or "rats://[IPV6]:PORT"), an address to
 * dial, and "rats://PEER-ID", the peer id in hex of a node that is
 * discoverable, looked for through the DHT. A trailing slash is allowed.
 * And a server, as a user writes one (parse_server()).
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "ratcast/session/media_network.h"

#include "librats/core/host_endpoint.h"

namespace ratcast {

using RatsUrl = std::variant<librats::HostEndpoint, PeerId>;

/// Nullopt for anything that is neither kind.
std::optional<RatsUrl> parse_rats_url(std::string_view url);

std::string to_string(const RatsUrl& url);

/// The port a ratcast-server listens on unless told otherwise.
inline constexpr uint16_t kServerPort = 7700;

/// A server as a user writes one: "HOST", "HOST:PORT" or "rats://HOST[:PORT]"
/// ("[IPV6]" for an IPv6 address), the port kServerPort if none is given.
/// Nullopt for anything else.
std::optional<librats::HostEndpoint> parse_server(std::string_view text);

}  // namespace ratcast
