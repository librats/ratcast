#include "ratcast/stream/fastest_transit.h"

#include <algorithm>

namespace ratcast {

void FastestTransit::add(Duration transit, Timestamp now) noexcept {
    if (!period_start_ || now - *period_start_ >= period_) {
        std::shift_left(period_least_.begin(), period_least_.end(), 1);
        period_least_.back() = transit;
        period_start_ = now;
    } else {
        period_least_.back() = std::min(*period_least_.back(), transit);
    }
    least_.reset();
    for (const std::optional<Duration>& least : period_least_) {
        if (least) least_ = std::min(least_.value_or(*least), *least);
    }
}

}  // namespace ratcast
