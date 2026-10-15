#include "ratcast/core/time.h"

namespace ratcast {

Timestamp SteadyClock::now() const noexcept {
    return std::chrono::time_point_cast<Duration>(std::chrono::steady_clock::now());
}

SteadyClock& SteadyClock::instance() noexcept {
    static SteadyClock clock;
    return clock;
}

}  // namespace ratcast
