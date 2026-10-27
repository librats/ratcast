#pragma once

/**
 * @file host_time.h
 * @brief The local clock's time at a host time: mach_absolute_time()'s
 * ticks, which Core Audio's and ScreenCaptureKit's stamps are in.
 */

#include <mach/mach_time.h>

#include <cstdint>

#include "ratcast/core/time.h"

namespace ratcast::macos {

/// When `host` was (or will be) on `clock`, by how far it is from now.
inline Timestamp at_host_time(Clock& clock, uint64_t host) {
    static const mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    const Timestamp now = clock.now();
    const uint64_t host_now = mach_absolute_time();
    const int64_t ticks = host >= host_now ? int64_t(host - host_now) : -int64_t(host_now - host);
    return now + Duration{ticks * int64_t(base.numer) / int64_t(base.denom) / 1000};
}

}  // namespace ratcast::macos
