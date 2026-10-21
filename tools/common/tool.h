#pragma once

/**
 * @file tool.h
 * @brief What the tools share: the names they parse, Ctrl-C, logs, clocks and
 * CPU meters.
 */

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "ratcast/core/time.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast::tools {

/// "av1", "h265" (or "hevc"), "h264".
std::optional<Codec> parse_codec(std::string_view name);

/// "1280x720".
std::optional<std::pair<uint32_t, uint32_t>> parse_size(std::string_view text);

/// From now on Ctrl-C (or SIGTERM) sets interrupted() instead of killing, and
/// a pipe whose reader went fails a write instead of killing.
void catch_interrupts();
bool interrupted() noexcept;

/// Library and codec logs: warnings and up, or everything with `verbose`.
void set_verbose(bool verbose);

/// The time of day as a Clock, for the test pattern's stamps: on two hosts
/// whose clocks are synchronised (NTP, PTP) a stamp made on one reads the same
/// on the other, so latency from capture to picture can be measured across
/// them. For stamps only: it may step, so never for pacing or timeouts.
class WallClock final : public Clock {
public:
    Timestamp now() const noexcept override;
    static WallClock& instance() noexcept;
};

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

private:
    using Wall = std::chrono::steady_clock;
    static double percent(Duration cpu, Wall::duration wall);

    Wall::time_point start_;
    Wall::time_point last_;
    Duration start_cpu_;
    Duration last_cpu_;
};

}  // namespace ratcast::tools
