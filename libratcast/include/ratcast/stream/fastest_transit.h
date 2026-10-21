#pragma once

/**
 * @file fastest_transit.h
 * @brief The least time frames took lately: where a receiver counts lateness
 * from.
 *
 * A frame's transit is its arrival, on the receiver's clock, less its capture
 * time, on the sender's: the clocks' offset and the path's delay together,
 * neither known alone. The least transit of late is the offset and the path's
 * least delay; how late a frame is counts from there.
 *
 * "Of late" is over `memory`, in a few periods, the oldest dropped as a new one
 * starts: a path whose delay rose for good is followed within `memory`, and one
 * that fell at once.
 */

#include <array>
#include <cstddef>
#include <optional>

#include "ratcast/core/time.h"
#include "ratcast/export.h"

namespace ratcast {

class RATCAST_EXPORT FastestTransit {
public:
    explicit FastestTransit(Duration memory) noexcept : period_(memory / kPeriods) {}

    /// A frame's transit, as of `now` (its arrival).
    void add(Duration transit, Timestamp now) noexcept;

    /// The least of late; none before the first frame.
    std::optional<Duration> least() const noexcept { return least_; }

private:
    static constexpr int64_t kPeriods = 4;

    Duration period_;
    std::array<std::optional<Duration>, kPeriods> period_least_{};  ///< the oldest first
    std::optional<Timestamp> period_start_;
    std::optional<Duration> least_;
};

}  // namespace ratcast
