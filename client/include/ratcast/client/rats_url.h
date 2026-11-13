#pragma once

/**
 * @file rats_url.h
 * @brief rats:// links: where a room, or a ratcast node, is reached.
 *
 * Three kinds:
 * - "rats://r/ROOM", a room's secret (RoomSecret: 22 characters, base64url):
 *   whoever has it meets the others in the room under a key of it
 *   (room_key()), at the servers and in the DHT — nobody's node, whoever
 *   comes first. A room made once has a secret of its own, a group's channel
 *   one of the group's (group.h). In the path, not the host's place, where
 *   URL parsers lowercase it — Qt's, as Android and macOS hand links to the
 *   app; "rats://ROOM", as links were written before, is taken too;
 * - "rats://PEER-ID", the peer id in hex of a node that is discoverable,
 *   looked for through the DHT (a sender of the tools');
 * - "rats://HOST:PORT" (or "rats://[IPV6]:PORT"), an address to dial: a
 *   member on the same network, with no server nor the internet.
 * A trailing slash is allowed. A link (RatsLink) may name servers too:
 * "rats://r/ROOM?via=HOST:PORT", where the room is met that its members may not
 * know of. And a server, as a user writes one (parse_server()).
 *
 * A link goes about as a web link too (to_web_link()): the website's page
 * that opens it in the app, "https://ratcast.app/r/#" and the link without
 * its "rats://" (a room's without its "r/" too) — clickable in every
 * messenger, which rats:// is not, and opening the app on Android by itself
 * (its App Links). What follows '#' never reaches the website's server.
 */

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ratcast/session/media_network.h"

#include "librats/core/host_endpoint.h"

namespace ratcast {

/// A room that is nobody's node: what lets one in, and the key it is met
/// under (room_key()).
struct RoomSecret {
    std::array<uint8_t, 16> bytes{};

    /// A new one: a room made once.
    static RoomSecret random();

    friend bool operator==(const RoomSecret&, const RoomSecret&) = default;
};

/// The key a room is met under, at the servers and in the DHT: of its
/// secret, which it does not tell.
std::string room_key(const RoomSecret& room);

using RatsUrl = std::variant<librats::HostEndpoint, PeerId, RoomSecret>;

/// Nullopt for anything that is no kind of them.
std::optional<RatsUrl> parse_rats_url(std::string_view url);

std::string to_string(const RatsUrl& url);

/// Where to go, and the servers the room is met at that a member may not know
/// of: "?via=…", one or more joined by '&' (any other parameter passed over).
struct RatsLink {
    RatsUrl url;
    std::vector<librats::HostEndpoint> via = {};

    bool operator==(const RatsLink&) const = default;
};

/// A RatsUrl with servers after it, if any, as a rats:// link or as a web link
/// (to_web_link(), its fragment percent-encoded or not); nullopt for anything
/// else, a via that is no server too.
std::optional<RatsLink> parse_rats_link(std::string_view link);

std::string to_string(const RatsLink& link);

/// The website's page that opens links in the app: a web link is this and the
/// rats:// link without its scheme (and a room's without its "r/").
inline constexpr std::string_view kWebLinkPrefix = "https://ratcast.app/r/#";

/// The link as a web link: "https://ratcast.app/r/#ROOM?via=…".
std::string to_web_link(const RatsLink& link);

/// The port a ratcast-server listens on unless told otherwise.
inline constexpr uint16_t kServerPort = 7700;

/// A server as a user writes one: "HOST", "HOST:PORT" or "rats://HOST[:PORT]"
/// ("[IPV6]" for an IPv6 address), the port kServerPort if none is given.
/// Nullopt for anything else.
std::optional<librats::HostEndpoint> parse_server(std::string_view text);

}  // namespace ratcast
