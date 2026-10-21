#pragma once

/**
 * @file pcr_pacer.h
 * @brief When each packet of a recorded MPEG-TS feed is due, from its PCR.
 *
 * A feed carries its own clock: the PCR, a 27 MHz count in the adaptation
 * field of some packets of one PID, every 40 ms or so. Played back as a live
 * encoder would send it, a packet with a PCR is due at that PCR, counted from
 * the first; the packets after it are due evenly at the rate of the interval
 * before (a constant-bitrate feed comes out exact; a variable one is put right
 * at the next PCR). A PCR that goes back, or leaps ahead more than a second
 * (a cut, a loop in the recording), is a discontinuity: time carries on at
 * the rate there was.
 */

#include <cstdint>
#include <optional>
#include <span>

#include "ratcast/core/time.h"
#include "ratcast/engine/ts_io.h"

namespace ratcast {

/// The PCR of `packet`, in 27 MHz ticks, if it carries one.
inline std::optional<uint64_t> read_pcr(std::span<const uint8_t, kTsPacketSize> packet) noexcept {
    const bool adaptation = (packet[3] & 0x20) != 0;
    if (packet[0] != kTsSyncByte || !adaptation || packet[4] < 7 || (packet[5] & 0x10) == 0) {
        return std::nullopt;
    }
    const uint64_t base = uint64_t{packet[6]} << 25 | uint64_t{packet[7]} << 17 |
                          uint64_t{packet[8]} << 9 | uint64_t{packet[9]} << 1 | packet[10] >> 7;
    const uint64_t extension = uint64_t{packet[10] & 1u} << 8 | packet[11];
    return base * 300 + extension;
}

inline uint16_t pid_of(std::span<const uint8_t, kTsPacketSize> packet) noexcept {
    return static_cast<uint16_t>((packet[1] & 0x1F) << 8 | packet[2]);
}

class PcrPacer {
public:
    /// When `packet`, the next of the feed, is due, from the first PCR on;
    /// zero before it.
    Duration due(std::span<const uint8_t, kTsPacketSize> packet) noexcept {
        ++since_;
        const std::optional<uint64_t> pcr = read_pcr(packet);
        if (!pcr || (pid_ && pid_of(packet) != *pid_)) return at_ + per_packet_ * since_;
        if (!last_pcr_) {
            pid_ = pid_of(packet);  // the first PCR: the clock starts
        } else {
            const uint64_t ticks = (*pcr + kWrap - *last_pcr_) % kWrap;
            const Duration step{static_cast<int64_t>(ticks / 27)};
            if (ticks > 0 && step <= kMaxStep) {
                per_packet_ = step / since_;
                at_ += step;
            } else {
                at_ += per_packet_ * since_;  // a discontinuity
            }
        }
        last_pcr_ = pcr;
        since_ = 0;
        return at_;
    }

private:
    static constexpr uint64_t kWrap = (uint64_t{1} << 33) * 300;
    static constexpr Duration kMaxStep = std::chrono::seconds(1);

    std::optional<uint16_t> pid_;  ///< the one whose PCR is followed
    std::optional<uint64_t> last_pcr_;
    Duration at_{};          ///< when the packet with the last PCR was due
    Duration per_packet_{};  ///< over the last interval
    int64_t since_ = 0;      ///< packets since the one with the last PCR
};

}  // namespace ratcast
