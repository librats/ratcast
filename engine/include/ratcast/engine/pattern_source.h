#pragma once

/**
 * @file pattern_source.h
 * @brief Synthetic pictures that carry their own capture time.
 *
 * A moving picture — a gradient that drifts and a bar that sweeps, in colour —
 * so that an encoder has motion to predict, with the capture time stamped into
 * a corner as an 8 x 8 grid of black and white blocks: 56 bits of
 * microseconds and a CRC-8. Blocks of that size survive any sensible
 * compression, so the time can be read back from the decoded picture on the
 * far side — which is what measuring glass-to-glass latency takes, and what
 * lets tests check pictures end to end without comparing pixels.
 */

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/video_frame.h"
#include "ratcast/engine/video_source.h"

namespace ratcast {

struct PatternConfig {
    PixelFormat format = PixelFormat::I420;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;
};

/// Picture number `index` of the pattern, stamped with `capture_time`.
/// Precondition: width and height of at least 64.
VideoFrame draw_pattern(const PatternConfig& config, uint64_t index, Timestamp capture_time);

/// The capture time stamped into `picture` by draw_pattern(); nullopt if the
/// stamp cannot be read (the CRC does not match).
std::optional<Timestamp> read_timestamp(const VideoFrame& picture);

/// The pattern as a VideoSource: a picture every frame interval, from a thread
/// of its own, stamped with the clock's time when drawing started.
class PatternSource final : public VideoSource {
public:
    explicit PatternSource(PatternConfig config, Clock& clock = SteadyClock::instance());
    ~PatternSource() override;

    Result<void> start(Sink sink) override;
    void stop() override;

private:
    void run(Sink sink);

    const PatternConfig config_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace ratcast
