#pragma once

/**
 * @file playback_meter.h
 * @brief How playback looks to a viewer: latency, stalls, the wait for the
 * first picture.
 *
 * Transport and decoder counters say what was lost; a viewer sees something
 * else — how late the pictures are and whether the picture stands still. The
 * meter is told about each picture as it is shown (or decoded) and keeps:
 *
 *   - the wait from start() to the first picture: joining a stream;
 *   - stalls: gaps between pictures longer than a threshold, counted and added
 *     up. Meaningful for sources that send at a steady rate (a test pattern, a
 *     film); a desktop sends only what changes, and its gaps are no stalls;
 *   - latency, capture to picture, for pictures whose capture time is known
 *     on this clock (the test pattern's stamp, on synchronised clocks), as a
 *     histogram: percentiles in fixed memory, whatever the run's length.
 *
 * Totals since start() and intervals (since the previous take_interval()), for
 * a summary and for a line a second. Thread-safe; picture() takes a mutex and
 * does no allocation.
 */

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>

#include "ratcast/core/time.h"

namespace ratcast {

/// Durations summarised: count, exact mean and maximum, percentiles within
/// 1/32 (3%) of the true value. Log-linear buckets, 32 per power of two, up
/// to 19 hours: 4 KiB whatever is recorded.
class LatencyHistogram {
public:
    void add(Duration value) noexcept;
    void clear() noexcept;

    uint64_t count() const noexcept { return count_; }
    Duration mean() const noexcept;
    Duration max() const noexcept { return max_; }
    /// The value a share `p` (0..1) of the samples lie at or below; zero when
    /// there are none.
    Duration percentile(double p) const noexcept;

private:
    static constexpr int kSubBits = 5;  ///< 32 buckets per power of two
    static constexpr int kSub = 1 << kSubBits;
    static constexpr int kBuckets = kSub * 32;  ///< microseconds below 2^36

    static size_t bucket(uint64_t us) noexcept;
    static uint64_t upper_bound(size_t bucket) noexcept;

    std::array<uint32_t, kBuckets> buckets_{};
    uint64_t count_ = 0;
    uint64_t sum_us_ = 0;
    Duration max_{};
};

struct PlaybackStats {
    uint64_t pictures = 0;
    /// From start() to the first picture; none yet. In a total only.
    std::optional<Duration> first_picture;
    uint64_t stalls = 0;       ///< gaps between pictures past the stall threshold
    Duration stalled{};        ///< those gaps, added up
    Duration longest_gap{};    ///< between two pictures, stall or not
    LatencyHistogram latency;  ///< capture to picture, where known
};

class PlaybackMeter {
public:
    /// A gap between pictures longer than `stall_after` is a stall: a viewer
    /// notices a picture held for about 100 ms.
    explicit PlaybackMeter(Clock& clock = SteadyClock::instance(),
                           Duration stall_after = std::chrono::milliseconds(100));

    /// Start measuring now: every count is cleared, and the wait for the first
    /// picture counts from here.
    void start();

    /// A picture is shown now; `latency` from its capture, if known.
    void picture(std::optional<Duration> latency = std::nullopt);

    /// Everything since start().
    PlaybackStats total() const;
    /// Everything since the previous call (or start()), then start a new interval.
    PlaybackStats take_interval();

private:
    Clock& clock_;
    const Duration stall_after_;
    mutable std::mutex mutex_;
    Timestamp started_{};
    std::optional<Timestamp> last_picture_;
    PlaybackStats total_;
    PlaybackStats interval_;
};

}  // namespace ratcast
