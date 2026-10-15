#pragma once

/**
 * @file time.h
 * @brief Media time: microseconds on a monotonic clock, and the clock as a seam.
 *
 * One unit everywhere inside libratcast. Codec time bases (90 kHz video,
 * 48 kHz audio, 1/fps) are converted at the codec boundary and nowhere else,
 * so arithmetic between a capture time, a deadline and a round trip never
 * needs a conversion — and the type system refuses to mix a Timestamp with a
 * bare integer.
 *
 * A Timestamp is a point on the local monotonic clock. It means nothing on
 * another host: a receiver compares remote capture times only with each other
 * (or through an estimated clock offset), never with its own now().
 */

#include <atomic>
#include <chrono>
#include <cstdint>

#include "ratcast/export.h"

namespace ratcast {

using Duration = std::chrono::microseconds;
using Timestamp = std::chrono::time_point<std::chrono::steady_clock, Duration>;

/// Where "now" comes from. Components take a Clock& instead of reading the
/// system clock themselves, so tests can drive them in virtual time.
class RATCAST_EXPORT Clock {
public:
    virtual ~Clock() = default;
    virtual Timestamp now() const noexcept = 0;
};

/// The real monotonic clock (std::chrono::steady_clock).
class RATCAST_EXPORT SteadyClock final : public Clock {
public:
    Timestamp now() const noexcept override;

    /// A process-wide instance, for components that are not given a clock.
    static SteadyClock& instance() noexcept;
};

/// A clock that moves only when told to. Thread-safe: one thread may advance
/// it while others read it.
class RATCAST_EXPORT ManualClock final : public Clock {
public:
    explicit ManualClock(Timestamp start = Timestamp{}) noexcept
        : now_us_(start.time_since_epoch().count()) {}

    Timestamp now() const noexcept override {
        return Timestamp{Duration{now_us_.load(std::memory_order_acquire)}};
    }

    void advance(Duration by) noexcept { now_us_.fetch_add(by.count(), std::memory_order_acq_rel); }

    void set(Timestamp to) noexcept {
        now_us_.store(to.time_since_epoch().count(), std::memory_order_release);
    }

private:
    std::atomic<Duration::rep> now_us_;
};

}  // namespace ratcast
