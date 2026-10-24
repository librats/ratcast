#include "ratcast/session/common_delay.h"

#include <algorithm>
#include <utility>

namespace ratcast {

bool CommonDelay::need(const PeerId& viewer, std::optional<Duration> latency, Timestamp now) {
    if (latency) {
        needs_[viewer] = *latency;
    } else {
        needs_.erase(viewer);
    }
    return update(now);
}

bool CommonDelay::update(Timestamp now) {
    if (needs_.size() < 2) {
        lower_since_.reset();
        return std::exchange(delay_, std::nullopt).has_value();
    }
    Duration most{};
    for (const auto& [viewer, latency] : needs_) most = std::max(most, latency);
    const Duration wanted = most + kMargin;

    if (!delay_ || wanted > *delay_) {
        delay_ = wanted;
        lower_since_.reset();
        return true;
    }
    if (wanted >= *delay_ - kMargin) {
        lower_since_.reset();
        return false;
    }
    if (!lower_since_) lower_since_ = now;
    if (now - *lower_since_ < kLowerAfter) return false;
    delay_ = wanted;
    lower_since_.reset();
    return true;
}

std::vector<PeerId> CommonDelay::viewers() const {
    std::vector<PeerId> viewers;
    viewers.reserve(needs_.size());
    for (const auto& [viewer, latency] : needs_) viewers.push_back(viewer);
    return viewers;
}

}  // namespace ratcast
