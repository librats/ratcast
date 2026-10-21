#pragma once

/**
 * @file ts_probe.h
 * @brief A stamped MPEG-TS feed and its meter: how a transport of TS feeds —
 * ratcast's data stream, SRT, librist — carries one, measured the same way
 * whichever it is.
 *
 * The probe is packets of one PID whose payloads carry the packet's number
 * and the time it was sent. The meter reads them where the transport hands
 * the feed on and keeps:
 *
 *   - latency, sent to received, when both ends' clocks agree (one host, or
 *     hosts synchronised by NTP or PTP);
 *   - loss: the numbers between the lowest and the highest seen that never
 *     came; duplicates and packets that came after a later one;
 *   - discontinuities: jumps of the continuity counter, the glitches a TS
 *     decoder sees (a burst of loss is one);
 *   - the wait from start() to the first packet, and the longest gap between
 *     two packets: an outage.
 *
 * The meter takes reads of any size and skips what is no probe packet.
 */

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "ratcast/client/playback_meter.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/ts_io.h"

namespace ratcast {

inline constexpr uint16_t kTsProbePid = 0x100;

/// Makes the probe's packets.
class TsProbeWriter {
public:
    /// Fill `packets`, a whole number of them, with the next ones, sent `now`.
    void fill(std::span<uint8_t> packets, Timestamp now) noexcept;

    uint64_t written() const noexcept { return next_; }

private:
    uint64_t next_ = 0;
};

struct TsProbeStats {
    uint64_t packets = 0;          ///< probe packets received, every copy
    uint64_t lost = 0;             ///< numbers never received (totals only)
    uint64_t duplicates = 0;       ///< copies of a packet already received
    uint64_t reordered = 0;        ///< came after a later one
    uint64_t discontinuities = 0;  ///< continuity counter jumps
    uint64_t foreign = 0;          ///< bytes that were no probe packet
    uint64_t bytes = 0;            ///< all that was read
    /// From start() to the first packet; none yet. In a total only.
    std::optional<Duration> first_packet;
    Duration longest_gap{};    ///< between two packets received
    LatencyHistogram latency;  ///< sent to received
};

class TsProbeMeter {
public:
    /// Received times come with the bytes; `clock` is what start() reads.
    explicit TsProbeMeter(Clock& clock = SteadyClock::instance());

    /// Start measuring now: every count is cleared.
    void start();

    /// Bytes read `received`, on the clock the probe was stamped with.
    void take(std::span<const uint8_t> bytes, Timestamp received);

    /// Everything since start().
    TsProbeStats total() const;
    /// Everything since the previous call (or start()), then start a new
    /// interval. Loss is counted in totals only.
    TsProbeStats take_interval();

private:
    /// Numbers remembered for duplicates: 13 s of a 5 Mbit/s feed. A packet
    /// older than that is dropped as a duplicate.
    static constexpr uint64_t kWindow = 1 << 16;

    void packet(std::span<const uint8_t> packet, Timestamp received);
    /// Whether `number` was seen already; marks it seen.
    bool seen(uint64_t number);

    Clock& clock_;
    mutable std::mutex mutex_;
    Timestamp started_{};
    TsPacketFinder finder_;
    std::optional<uint64_t> lowest_;
    uint64_t highest_ = 0;
    uint64_t unique_ = 0;
    std::vector<bool> seen_ = std::vector<bool>(kWindow);  ///< by number % kWindow
    std::optional<uint8_t> continuity_;
    std::optional<Timestamp> last_packet_;
    TsProbeStats total_;
    TsProbeStats interval_;
};

}  // namespace ratcast
