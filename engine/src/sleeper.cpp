// Real-time waits where the system's own sleep is as close as it goes.

#include "sleeper.h"

#include <thread>

namespace ratcast {

Sleeper::Sleeper() = default;

Sleeper::~Sleeper() = default;

void Sleeper::until(std::chrono::steady_clock::time_point when) {
    std::this_thread::sleep_until(when);
}

}  // namespace ratcast
