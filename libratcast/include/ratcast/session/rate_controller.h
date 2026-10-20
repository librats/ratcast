#pragma once

/**
 * @file rate_controller.h
 * @brief The bitrate a live stream's encoder aims at, from what its paths say.
 *
 * A sender that puts more on a path than the path carries does not lose a
 * little: the queue in front of the path grows until every frame waits out its
 * lifetime and expires. So the encoder follows the paths to the stream's
 * subscribers, which librats samples every 100 ms or so (PathStats):
 *
 *   - It starts low, so that the first keyframe is small, and ramps up ×1.5 a
 *     step while nothing pushes back.
 *   - It comes down at once when a path is overused: live messages expired, a
 *     subscriber's queue filled up, or a message waited in front of the path
 *     for more than a quarter of the stream's lifetime in two samples running
 *     (a keyframe alone drains within one). It cuts to 85% of what the path is
 *     known to carry, or of the target when that is not known — never below
 *     half the target at once — and not again until the frames sent before
 *     the cut are gone: a lifetime and a round trip later.
 *   - Not while the transport is still finding the path (BBR's Startup): its
 *     estimate is then how far it got, a queue measured by it is no queue,
 *     and the first frames — a joining viewer's keyframe — expire waiting for
 *     its window to open, not for a path the stream overfills. A stream that
 *     does overfill the path ends Startup within a few round trips; a full
 *     queue counts throughout.
 *   - It remembers where a full path (below) filled up, climbs back quickly
 *     to 85% of that and carefully on, +8% a step, ramping again only once it
 *     is well past that point with no trouble: the path has grown. A cut with
 *     the path not known to be full — frames expired on a path that loses
 *     packets — leaves no such mark.
 *   - It never climbs above what a path is known to carry, with 10% to spare.
 *     A path is known only from an estimate taken while the sender kept it
 *     busy, two samples running (a keyframe keeps it busy for a moment), and
 *     no longer growing (BBR's Startup grows it every round trip). One taken
 *     while the path was not busy is a floor, and finding out how much more
 *     there is takes sending more, which is what climbing does. A cut also
 *     goes by what a busy path delivered since the last sample: the estimate
 *     is the best of the last few seconds and takes that long to come down
 *     after the path narrows. The round trip is no signal: jitter alone keeps
 *     the smoothed one tens of milliseconds above the least.
 *
 * Upward steps are a second apart, the first two seconds after a cut, and at
 * least 5%: an encoder that cannot change its rate in flight starts over, with
 * a keyframe, for each.
 *
 * Not thread-safe: MediaSession locks around it.
 */

#include <cstdint>
#include <optional>

#include "ratcast/core/time.h"
#include "ratcast/export.h"

#include "librats/core/types.h"

namespace ratcast {

using PathStats = librats::PathStats;

/// The bitrates rate control keeps a stream in, bits per second: it starts at
/// `start` and moves between `min` and `max` as the paths allow.
struct BitrateLimits {
    uint32_t min = 0;
    uint32_t start = 0;
    uint32_t max = 0;

    friend bool operator==(const BitrateLimits&, const BitrateLimits&) = default;
};

/// Whether 0 < min <= start <= max.
RATCAST_EXPORT bool is_valid(const BitrateLimits& limits) noexcept;

/// What one sample of a path says, for rate control.
struct PathSignal {
    /// The transport is still finding the path — pacing at more than twice its
    /// estimate, as BBR's Startup does (2.77×; at most 1.25× after it). The
    /// sample then says how far it got, not what the path carries: none of
    /// the figures below is about the path, and `capacity` and `delivered`
    /// stay empty.
    bool starting = false;
    /// Live messages to the peer expired since the previous sample.
    bool expired = false;
    /// How long a new message waits before it is sent: the lesser of the last
    /// two samples, so a keyframe draining is not taken for a growing queue.
    Duration queue_delay{};
    Duration rtt{};
    /// What the path carries for media — the congestion controller's estimate
    /// less forward error correction — when the path is full: busy in this
    /// sample and the last, with an estimate that has stopped growing. nullopt
    /// otherwise: the estimate is a floor.
    std::optional<uint64_t> capacity;
    /// What the path delivered for media since the previous sample, when it
    /// was busy in both: what it carries now, where the estimate (the best of
    /// the last few seconds) still remembers it wider. nullopt otherwise: an
    /// idle path delivers what it is given.
    std::optional<uint64_t> delivered;
};

/// Turns one peer's path samples into signals: the counters in a sample run
/// from the connection's start, a signal is about the time since the last.
class RATCAST_EXPORT PathMonitor {
public:
    PathSignal update(const PathStats& sample, Timestamp now);

private:
    std::optional<PathStats> last_;
    Timestamp last_at_{};
    Duration last_queue_delay_{};
};

class RATCAST_EXPORT RateController {
public:
    /// `limits` must be valid; `lifetime` is the stream's.
    RateController(BitrateLimits limits, Duration lifetime) noexcept;

    /// What the encoder should aim at now.
    uint32_t target() const noexcept { return target_; }

    /// Whether `signal` reports a path this stream overuses.
    bool overused(const PathSignal& signal) const noexcept;

    /// A path is overused (see overused(), or a subscriber's queue was full);
    /// `path` is its latest signal. Returns the new target if it changed.
    std::optional<uint32_t> on_overuse(Timestamp now, const PathSignal& path) noexcept;

    /// A path reported no overuse. `capacity` is the least any path of the
    /// stream is known to carry. Returns the new target if it changed.
    std::optional<uint32_t> on_clear(Timestamp now, std::optional<uint64_t> capacity) noexcept;

    /// Back to `start`, forgetting the paths: nobody is watching any more.
    /// Returns the new target if it changed.
    std::optional<uint32_t> restart() noexcept;

private:
    std::optional<uint32_t> set_target(double bits_per_second) noexcept;

    const BitrateLimits limits_;
    const Duration lifetime_;
    uint32_t target_;
    /// The rate at which a full path last filled up; forgotten once well past
    /// it.
    std::optional<double> ceiling_;
    std::optional<Timestamp> last_cut_;
    std::optional<Timestamp> last_raise_;
};

}  // namespace ratcast
