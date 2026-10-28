// A thread of real time: its timers have no leeway, which those of a thread
// of a background process's class have plenty of.

#include "ratcast/engine/real_time.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>

#include <cstdint>

#include "librats/util/logger.h"

namespace ratcast {

void keep_time_on_this_thread() {
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    const auto ticks = [&](uint64_t ms) {
        return uint32_t(ms * 1'000'000 * timebase.denom / timebase.numer);
    };
    // No period: woken when due, not on a cycle. Up to 10 ms of work each
    // time, done within 20, and preempted past it rather than demoted.
    thread_time_constraint_policy_data_t policy{};
    policy.period = 0;
    policy.computation = ticks(10);
    policy.constraint = ticks(20);
    policy.preemptible = TRUE;
    const kern_return_t result = thread_policy_set(
        pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (result != KERN_SUCCESS) {
        LOG_WARN("engine", "waits may end late: no real-time thread (" << result << ")");
    }
}

}  // namespace ratcast
