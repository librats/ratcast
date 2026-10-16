#pragma once

/**
 * @file tool.h
 * @brief What ratcast-send and ratcast-recv share: their librats node, the
 * names they parse, Ctrl-C.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ratcast/session/media_network.h"
#include "ratcast/stream/stream_description.h"

#include "librats/node/config.h"
#include "librats/node/node.h"

namespace ratcast::tools {

/// The handshake protocol of every ratcast node: tools of another protocol
/// version do not connect at all.
inline constexpr const char* kProtocol = "ratcast/0";

/// A node for a tool: listening on `port` (0 for any) if `listen`, with a
/// stable identity when `data_dir` is not empty.
librats::NodeConfig node_config(uint16_t port, bool listen, const std::string& data_dir);

/// Find peers through the DHT under `key`, announcing ourselves under it too:
/// how a viewer finds a sender by its peer id alone. Takes a minute or two to
/// converge on the open internet.
void add_discovery(librats::Node& node, const std::string& key, const std::string& data_dir);

/// The DHT key a sender is found under.
std::string discovery_key(const PeerId& sender);

/// "host:port" or "[ipv6]:port".
std::optional<std::pair<std::string, uint16_t>> parse_endpoint(std::string_view text);

/// "av1", "h265" (or "hevc"), "h264".
std::optional<Codec> parse_codec(std::string_view name);

/// "1280x720".
std::optional<std::pair<uint32_t, uint32_t>> parse_size(std::string_view text);

/// From now on Ctrl-C (or SIGTERM) sets interrupted() instead of killing.
void catch_interrupts();
bool interrupted() noexcept;

/// Library and codec logs: warnings and up, or everything with `verbose`.
void set_verbose(bool verbose);

}  // namespace ratcast::tools
