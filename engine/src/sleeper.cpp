// Real-time waits where the system's own sleep is as close as it goes: on
// Linux as it is, on macOS from a thread of real time.

#include "sleeper.h"

#include <thread>

#include "ratcast/engine/real_time.h"

namespace ratcast {

Sleeper::Sleeper() { keep_time_on_this_thread(); }

Sleeper::~Sleeper() = default;

void Sleeper::until(std::chrono::steady_clock::time_point when) {
    std::this_thread::sleep_until(when);
}

}  // namespace ratcast
