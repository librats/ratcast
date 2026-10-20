#pragma once

/**
 * @file sample_clock.h
 * @brief Capture times for a sound device's blocks.
 *
 * A block arrives when the device has filled it, a little late by however
 * long the wake-up took; stamping each by the time it arrived would make the
 * times jitter where the samples themselves run on without a gap. So the
 * times run on too — each block starts where the one before ended — and are
 * only drawn gently towards what arrival says, since the device's clock drifts
 * from the system's. A block far from where it should be (the device stalled,
 * blocks were lost) starts the count afresh.
 */

#include <cstddef>
#include <cstdint>
#include <optional>

#include "ratcast/core/time.h"
#include "ratcast/engine/audio_frame.h"

namespace ratcast {

class SampleClock {
public:
    /// How far arrival may be from the count before the count starts afresh.
    static constexpr Duration kResync = std::chrono::milliseconds(50);
    /// How much of the difference each block takes up.
    static constexpr int kSmoothing = 16;

    explicit SampleClock(uint32_t sample_rate) noexcept : sample_rate_(sample_rate) {}

    /// The capture time of the first sample of a block of `length` samples
    /// per channel that arrived complete at `now`.
    Timestamp stamp(size_t length, Timestamp now) noexcept {
        const Duration duration = audio_duration(length, sample_rate_);
        const Timestamp arrived = now - duration;
        Timestamp start = arrived;
        if (next_ && std::chrono::abs(arrived - *next_) < kResync)
            start = *next_ + (arrived - *next_) / kSmoothing;
        next_ = start + duration;
        return start;
    }

    void reset() noexcept { next_.reset(); }

private:
    const uint32_t sample_rate_;
    std::optional<Timestamp> next_;  ///< where the next block starts, by the count
};

}  // namespace ratcast
