#pragma once

/**
 * @file virtual_output.h
 * @brief An audio output without a device: the tone source's counterpart.
 *
 * It pulls sound in real time from a thread of its own, a block every
 * `block`, as a device would, and hands what it pulled to an optional tap:
 * what a headless player plays into, and what tests listen to. The sound is
 * heard, as far as Pull is told, `latency` after it is pulled.
 */

#include <atomic>
#include <functional>
#include <mutex>
#include <span>
#include <thread>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/audio_frame.h"

namespace ratcast {

struct VirtualOutputConfig {
    AudioFormat format{48'000, 2};
    /// How much sound is pulled at a time.
    Duration block = std::chrono::milliseconds(10);
    /// From a pull to the sound being "heard": a device's buffering.
    Duration latency = std::chrono::milliseconds(10);
};

class VirtualOutput final : public AudioOutput {
public:
    /// What was pulled, on the output's thread: the samples and the time
    /// they are heard.
    using Tap = std::function<void(std::span<const float> samples, Timestamp play_time)>;

    explicit VirtualOutput(VirtualOutputConfig config, Tap tap = {},
                           Clock& clock = SteadyClock::instance());
    ~VirtualOutput() override;

    const AudioFormat& format() const noexcept override { return config_.format; }
    Result<void> start(Pull pull, EndHandler ended) override;
    void stop() override;

private:
    void run(Pull pull);

    const VirtualOutputConfig config_;
    const Tap tap_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace ratcast
