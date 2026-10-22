#pragma once

/**
 * @file qpc.h
 * @brief The system's stamps on what devices hand out — WASAPI's packets,
 * Windows.Graphics.Capture's frames — on the local clock.
 *
 * They are the performance counter in 100 ns units: how long ago one was is
 * how far it is from the counter now, which takes it onto any Clock.
 */

#include <windows.h>

#include <cstdint>

#include "ratcast/core/time.h"

namespace ratcast::win {

/// The performance counter now, in 100 ns units.
inline int64_t qpc_now() noexcept {
    LARGE_INTEGER count;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return count.QuadPart / frequency.QuadPart * 10'000'000 +
           count.QuadPart % frequency.QuadPart * 10'000'000 / frequency.QuadPart;
}

inline Duration from_hundreds_of_ns(int64_t count) noexcept { return Duration{count / 10}; }

/// When `stamp` (the counter, in 100 ns units) was on `clock`.
inline Timestamp at_qpc(int64_t stamp, Clock& clock) noexcept {
    return clock.now() - from_hundreds_of_ns(qpc_now() - stamp);
}

}  // namespace ratcast::win
