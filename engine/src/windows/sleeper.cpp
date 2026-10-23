// Real-time waits on a high-resolution waitable timer: Sleep() steps in the
// system timer's tick, 15.6 ms by default.

#include "sleeper.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <thread>

namespace ratcast {

Sleeper::Sleeper()
    : timer_(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS)) {}

Sleeper::~Sleeper() {
    if (timer_) CloseHandle(timer_);
}

void Sleeper::until(std::chrono::steady_clock::time_point when) {
    const auto left = when - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return;
    LARGE_INTEGER due;  // negative: relative, in 100 ns
    due.QuadPart = -std::max<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100, 1);
    if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer_, INFINITE);
        return;
    }
    std::this_thread::sleep_for(left);  // before Windows 10 1803: as close as Sleep() goes
}

}  // namespace ratcast
