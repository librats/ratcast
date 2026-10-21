#pragma once

/**
 * @file delivery_buffer.h
 * @brief A data stream's frames handed on in order, at a fixed delay.
 *
 * Bytes ratcast does not look into — an MPEG-TS feed — go on to whatever takes
 * them (a socket, a file, a demuxer) as they went in: in order and evenly. The
 * network delivers them out of order, repairs what it can within the stream's
 * lifetime, and gives up on the rest. The buffer holds each frame until its
 * time — its capture time, plus the fastest transit of late (the clocks'
 * offset and the path's least delay), plus a fixed `latency` — and hands it on
 * then. A frame not there when one after it is due is lost: skipped, never
 * waited for, and dropped as late if it comes after all. That is SRT's
 * timestamp-based delivery with too-late drop, and librist's buffer; `latency`
 * is what both call it, the time repair has to work in.
 *
 * Codec-agnostic: it reads frame numbers and capture times only. Not
 * thread-safe.
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include "ratcast/core/time.h"
#include "ratcast/export.h"
#include "ratcast/stream/fastest_transit.h"
#include "ratcast/stream/frame.h"

namespace ratcast {

class RATCAST_EXPORT DeliveryBuffer {
public:
    struct Config {
        /// Frames are handed on this long after the fastest would be. No use
        /// beyond the stream's lifetime less the path's delay: past it the
        /// network has given up on what is missing.
        Duration latency = std::chrono::milliseconds(120);
        /// How long the fastest transit is remembered.
        Duration memory = std::chrono::seconds(10);
        /// Frames held at most; past it the earliest go on at once.
        size_t max_frames = 8192;
    };

    struct Stats {
        uint64_t received = 0;   ///< frames pushed
        uint64_t delivered = 0;  ///< handed on
        uint64_t lost = 0;       ///< frame numbers skipped: not there in time
        uint64_t late = 0;       ///< arrived after their time, or twice
        uint64_t early = 0;      ///< handed on before their time, for room
    };

    explicit DeliveryBuffer(Config config) noexcept;

    /// Forget everything and start again with the next frame.
    void reset() noexcept;

    /// Accept `frame`, received at `now`.
    void push(EncodedFrame frame, Timestamp now);

    /// The next frame in order, if its time has come at `now`. Call again
    /// until none: several may be due at once.
    std::optional<EncodedFrame> pop(Timestamp now);

    /// When the next frame held is due; none while none is held.
    std::optional<Timestamp> next_due() const noexcept;

    const Stats& stats() const noexcept { return stats_; }

private:
    Timestamp due(const EncodedFrame& frame) const noexcept;

    Config config_;
    Stats stats_;
    FastestTransit fastest_;
    std::deque<EncodedFrame> frames_;         ///< sorted by number, all >= next_number_
    std::optional<FrameNumber> next_number_;  ///< once one was handed on
};

}  // namespace ratcast
