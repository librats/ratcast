#pragma once

/**
 * @file tone_source.h
 * @brief A synthetic sound, the audio counterpart of the test pattern.
 *
 * A sine tone, continuous from one block to the next, its samples computed
 * from their index alone: block N of a tone is the same wherever and whenever
 * it is drawn, so tests can tell what a codec or a network did to it.
 */

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/audio_frame.h"
#include "ratcast/engine/audio_source.h"

namespace ratcast {

struct ToneConfig {
    AudioFormat format{48'000, 1};
    float frequency = 440.0f;  ///< Hz
    float level = 0.25f;       ///< peak, of full scale
    /// How much sound ToneSource hands out at a time, as a device would.
    Duration block = std::chrono::milliseconds(10);
};

/// `length` samples per channel of the tone, from sample `first` on, captured
/// at `capture_time`; every channel the same.
AudioFrame draw_tone(const ToneConfig& config, uint64_t first, size_t length,
                     Timestamp capture_time);

/// The tone as an AudioSource: a block every `block`, from a thread of its
/// own, in real time, each as its last sample is due. The first sample is
/// stamped with the clock's time when the source started and the rest follow
/// on from it, as a device's do. It never ends by itself.
class ToneSource final : public AudioSource {
public:
    explicit ToneSource(ToneConfig config, Clock& clock = SteadyClock::instance());
    ~ToneSource() override;

    const AudioFormat& format() const noexcept override { return config_.format; }
    Result<void> start(Sink sink, EndHandler ended) override;
    void stop() override;

private:
    void run(Sink sink);

    const ToneConfig config_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace ratcast
