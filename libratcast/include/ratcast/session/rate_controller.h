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
 * Streams that go to the same peer share its path: each is told of its part
 * of what the path carries (share_of()), in proportion to the most it may
 * take — a screen of 8 Mbit/s beside a camera of 2 takes four fifths. With
 * parts in proportion to the most, a stream gets more than its most only on a
 * path that carries more than all of them together, so none is left over for
 * the others. All of them hear an overused path, and each cuts to its part.
 *
 * Not thread-safe: MediaSession locks around it.
 */

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

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
    /// The time since the previous sample, zero for the first. Samples come
    /// every 100 ms or so; one much later than that is of a node that was not
    /// running in between, whose frames expired waiting for it, not for a
    /// path.
    Duration since{};
    /// How long a new message waits before it is sent: the lesser of the last
    /// two samples, so a keyframe draining is not taken for a growing queue.
    Duration queue_delay{};
    Duration rtt{};
    /// How far the smoothed round trip is above the least it was over the last
    /// ten seconds: a queue building beyond the sender, where the round trip
    /// itself says nothing (a jittery path's stays well above its least ever).
    Duration queue_rise{};
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
    /// What the path delivered for media over the last second or so, busy or
    /// not: its part of an uplink it shares with others (shared_uplink.h). A
    /// second, so that a moment's stall of the acknowledgements — a full
    /// queue's losses — does not read as an uplink that carries little.
    /// nullopt for the first sample, which has nothing to count from.
    std::optional<uint64_t> carried;
};

/// `path` as one of the streams sharing it sees it: `share` (0–1] of what it
/// carries and delivered; the rest is the path's, the same for every stream.
RATCAST_EXPORT PathSignal share_of(const PathSignal& path, double share) noexcept;

/// Turns one peer's path samples into signals: the counters in a sample run
/// from the connection's start, a signal is about the time since the last.
class RATCAST_EXPORT PathMonitor {
public:
    PathSignal update(const PathStats& sample, Timestamp now);

private:
    std::optional<PathStats> last_;
    Timestamp last_at_{};
    Duration last_queue_delay_{};
    /// Media bytes delivered (repairs left out) since the connection opened,
    /// as of each sample of the last second, and the one before them.
    std::deque<std::pair<Timestamp, uint64_t>> media_;
    /// The smoothed round trips of the last ten seconds that no later one is
    /// below: the least of them first.
    std::deque<std::pair<Timestamp, Duration>> least_rtts_;
};

class RATCAST_EXPORT RateController {
public:
    /// `limits` must be valid; `lifetime` is the stream's.
    RateController(BitrateLimits limits, Duration lifetime) noexcept;

    /// What the encoder should aim at now.
    uint32_t target() const noexcept { return target_; }
    const BitrateLimits& limits() const noexcept { return limits_; }

    /// Whether `signal` reports a path this stream overuses.
    bool overused(const PathSignal& signal) const noexcept;

    /// A path is overused (see overused(), or a subscriber's queue was full);
    /// `path` is its latest signal. Returns the new target if it changed.
    std::optional<uint32_t> on_overuse(Timestamp now, const PathSignal& path) noexcept;

    /// A path reported no overuse. `capacity` is the least any path of the
    /// stream is known to carry. Returns the new target if it changed.
    std::optional<uint32_t> on_clear(Timestamp now, std::optional<uint64_t> capacity) noexcept;

    /// Each copy of the stream is known to have at most `capacity` (its part
    /// of an uplink found full, or of one that takes another copy): the target
    /// comes down at once to where a raise would stop. Returns the new target
    /// if it changed.
    std::optional<uint32_t> limit(uint64_t capacity) noexcept;

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
