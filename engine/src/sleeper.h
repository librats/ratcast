// Waits for the threads that keep real time — the stand-in sources and
// output, a file played by its PCR. On Windows a wait ends on the system
// timer's tick, 15.6 ms by default, so there they are on a high-resolution
// waitable timer, which ends this one wait on time and leaves the process'
// timer as it is. On macOS a Sleeper makes the thread it is made on one of
// real time (keep_time_on_this_thread()), whose waits are not let end late.

#pragma once

#include <chrono>

namespace ratcast {

/// Sleeps that end within a fraction of a millisecond of when asked. Each
/// thread waits on one of its own, made on that thread.
class Sleeper {
public:
    Sleeper();
    ~Sleeper();
    Sleeper(const Sleeper&) = delete;
    Sleeper& operator=(const Sleeper&) = delete;

    void until(std::chrono::steady_clock::time_point when);

private:
    /// Windows' waitable timer (a HANDLE); unused elsewhere.
    [[maybe_unused]] void* timer_ = nullptr;
};

}  // namespace ratcast
