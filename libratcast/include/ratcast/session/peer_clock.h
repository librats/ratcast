#pragma once

/**
 * @file peer_clock.h
 * @brief Another node's clock against this one's, from round trips.
 *
 * A probe leaves at `sent` on this clock; the peer stamps it `at` on its own
 * and sends it back, here at `received`. Were the two ways as long, the peer
 * stamped it halfway there, and this clock less the peer's is
 * (sent + received) / 2 - at — wrong by half of however much longer one way
 * took than the other: time in a queue, mostly. So the round trip that took
 * least says it best, and the clock is the one of the least of the last
 * kWindow; two clocks drift apart by a millisecond in that time at the most.
 *
 * Not thread-safe.
 */

#include <deque>
#include <optional>

#include "ratcast/core/time.h"
#include "ratcast/export.h"

namespace ratcast {

class RATCAST_EXPORT PeerClock {
public:
    /// How long a round trip counts.
    static constexpr Duration kWindow = std::chrono::seconds(10);

    /// A probe that left at `sent` (this clock), stamped `at` by the peer (its
    /// clock) and back at `received`.
    void add(Timestamp sent, Timestamp at, Timestamp received);

    /// This clock less the peer's: what a time of the peer's is here, less
    /// that time. None before the first probe came back.
    std::optional<Duration> offset() const noexcept;

    /// The round trip offset() comes from.
    std::optional<Duration> round_trip() const noexcept;

private:
    struct Sample {
        Timestamp received{};
        Duration round_trip{};
        Duration offset{};
    };
    /// Each the least since it came back, so the front is the least of all.
    std::deque<Sample> least_;
};

}  // namespace ratcast
