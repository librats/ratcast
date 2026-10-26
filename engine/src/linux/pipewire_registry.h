#pragma once

/**
 * @file pipewire_registry.h
 * @brief The nodes on the user's PipeWire daemon, as its registry lists them:
 * what the cameras and the sound devices are picked from.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "ratcast/core/result.h"

namespace ratcast::pipewire {

/// A node, by what it says of itself.
struct NodeInfo {
    uint32_t id = 0;
    std::string media_class;  ///< "Audio/Sink", "Video/Source", …
    std::string role;         ///< media.role: "Camera", or ""
    std::string name;         ///< node.name: what it is known by from one run to the next
    /// node.description, else node.nick, else ""; for a UI.
    std::string description;
};

/// The nodes there are now: one look at the registry, on the calling thread,
/// once the daemon says it has listed everything. Fails with Unavailable when
/// there is no daemon, or it does not answer in a moment.
Result<std::vector<NodeInfo>> list_nodes();

}  // namespace ratcast::pipewire
