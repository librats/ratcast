#pragma once

/**
 * @file rats_url.h
 * @brief rats:// links: where a ratcast node is reached.
 *
 * Two kinds: "rats://HOST:PORT" (or "rats://[IPV6]:PORT"), an address to
 * dial, and "rats://PEER-ID", the peer id in hex of a node that is
 * discoverable, looked for through the DHT. A trailing slash is allowed.
 */

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

}  // namespace ratcast
