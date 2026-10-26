#pragma once

/**
 * @file camera_choice.h
 * @brief Whose cameras a side shows when it shows only so many: the last to
 * speak (last-N).
 *
 * In a room past a few members, every camera received is a decoder and a
 * stream's worth of downlink each. A side then shows the cameras of the last
 * few to speak, and the rest by name: a camera shown stays until someone whose
 * is not speaks a moment (`keep`) after its member last did — never for one
 * who has not spoken at all, nor while its member talks on — so that what is
 * shown follows the talk and does not flicker between two speaking at once.
 */

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ratcast/core/time.h"
#include "ratcast/session/media_network.h"

namespace ratcast {

/// A camera offered, as a side choosing what to show sees it.
struct CameraOffer {
    PeerId peer{};
    std::optional<Timestamp> spoke{};  ///< when its voice was last heard; never: none
    bool shown = false;                ///< shown now
};

/// How long after its member last spoke a camera shown keeps its place.
inline constexpr Duration kKeepCamera = std::chrono::seconds(2);

/// The peers of `offers` whose cameras are shown: all of them if they are no
/// more than `most` (0: no limit), else the `most` last to speak. A camera
/// shown keeps its place against one that never spoke, or spoke no later
/// than `keep` after its member; among those that never spoke, the order of
/// `offers` decides.
std::vector<PeerId> choose_cameras(std::span<const CameraOffer> offers, size_t most,
                                   Duration keep = kKeepCamera);

}  // namespace ratcast
