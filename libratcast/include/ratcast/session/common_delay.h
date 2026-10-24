#pragma once

/**
 * @file common_delay.h
 * @brief The delay all of a sender's viewers play its share at, so that each
 * shows a frame when the others do.
 *
 * Each viewer needs some time from a frame's capture to playing it — its
 * path's delay, its buffer for late frames, its decoder — and tells the
 * sender, on the sender's clock (peer_clock.h). Played each as soon as it
 * can, a film is seen at as many moments as there are viewers, and a room
 * laughs at different times. Played all at the most any of them needs, and a
 * margin, it is seen together.
 *
 * The delay rises at once as a viewer needs more: until then that one plays
 * late. It comes down only once every viewer has needed at least kMargin less
 * for kLowerAfter: each change is a jump in every viewer's playout — sound
 * made up or dropped, pictures held or hurried — not worth it for a few
 * milliseconds, or for a moment. With one viewer there is nobody to be in step
 * with, and no delay.
 *
 * Not thread-safe: MediaSession locks around it.
 */

#include <optional>
#include <unordered_map>
#include <vector>

#include "ratcast/core/time.h"
#include "ratcast/export.h"
#include "ratcast/session/media_network.h"

namespace ratcast {

class RATCAST_EXPORT CommonDelay {
public:
    /// Over the most any viewer needs, for a need that grows a little.
    static constexpr Duration kMargin = std::chrono::milliseconds(10);
    /// How long the needs must stay lower for the delay to come down.
    static constexpr Duration kLowerAfter = std::chrono::seconds(5);

    /// `viewer` needs `latency` from capture to played, on this node's clock;
    /// nullopt: it no longer plays in step. Returns whether the delay changed.
    bool need(const PeerId& viewer, std::optional<Duration> latency, Timestamp now);

    /// Time passing. Returns whether the delay changed: it came down, the
    /// viewers having needed less for long enough.
    bool update(Timestamp now);

    /// None with fewer than two viewers.
    std::optional<Duration> delay() const noexcept { return delay_; }

    /// Whether `viewer` said what it needs, and plays in step.
    bool plays(const PeerId& viewer) const { return needs_.contains(viewer); }

    std::vector<PeerId> viewers() const;

private:
    std::unordered_map<PeerId, Duration, PeerId::Hash> needs_;
    std::optional<Duration> delay_;
    std::optional<Timestamp> lower_since_;  ///< when the needs fell below it
};

}  // namespace ratcast
