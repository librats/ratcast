#pragma once

/**
 * @file frame_pacer.h
 * @brief Holding a source to a frame rate.
 *
 * A compositor sends a picture whenever the screen changes, which on a 144 Hz
 * display is up to 144 a second; the stream wants no more than, say, 60. The
 * pacer lets a picture through once per interval of the target rate, keeping
 * to a grid so that the rate comes out right on average whatever the source
 * runs at, and taking pictures a little early so that jitter in a source that
 * runs at just the target rate does not drop every other one.
 */

#include <algorithm>
#include <cstdint>
#include <optional>

#include "ratcast/core/time.h"

namespace ratcast {

class FramePacer {
public:
    /// At most `max_rate` pictures a second; 0 lets every picture through.
    explicit FramePacer(uint32_t max_rate) noexcept
        : interval_(max_rate > 0 ? Duration{1'000'000 / max_rate} : Duration::zero()),
          slack_(interval_ / 4) {}

    /// Whether a picture that arrived at `now` should go on.
    bool admit(Timestamp now) noexcept {
        if (next_ && now + slack_ < *next_) return false;
        // On the grid while the source keeps up; from now on when it fell behind.
        // Either way the grid is laid a slack early, as if this picture were
        // that late: the next may then come early by the slack and late by as
        // much again — jitter of twice the slack — and still go.
        const Timestamp from_now = now + interval_ - slack_;
        next_ = next_ ? std::max(*next_ + interval_, from_now) : from_now;
        return true;
    }

private:
    Duration interval_;
    Duration slack_;  ///< how early a picture may be and still go
    std::optional<Timestamp> next_;
};

}  // namespace ratcast
