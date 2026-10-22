#pragma once

/**
 * @file cpu_meter.h
 * @brief The CPU the process uses, as a share of one core.
 */

#include <chrono>

#include "ratcast/core/time.h"

namespace ratcast {

/// CPU time the process has used so far, every thread, user and system.
Duration process_cpu_time();

/// The process's CPU use as a share of one core (100 = one core busy): over
/// each interval, and since it was made.
class CpuMeter {
public:
    CpuMeter();
    /// Since the previous call (or since made).
    double take_interval();
    double total() const;

    /// `cpu` time used over `wall` time, as a share of one core.
    static double percent(Duration cpu, Duration wall);

private:
    using Wall = std::chrono::steady_clock;

    Wall::time_point start_;
    Wall::time_point last_;
    Duration start_cpu_;
    Duration last_cpu_;
};

}  // namespace ratcast
