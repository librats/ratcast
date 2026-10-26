#pragma once

/**
 * @file switchable_output.h
 * @brief An output whose device can be changed while it plays.
 *
 * A SwitchableOutput stands for a device to whoever plays into it — a mixer,
 * a player — and hands what they pull to the device it holds now. Switched to
 * another, the one it held is stopped and the new one started with the same
 * pull, so that what plays goes on there, told the new device's play times,
 * and nobody above it has to start again. Thread-safe.
 */

#include <memory>
#include <mutex>

#include "ratcast/core/result.h"
#include "ratcast/engine/audio_device.h"

namespace ratcast {

class SwitchableOutput final : public AudioOutput {
public:
    /// Precondition: `device` is not null.
    explicit SwitchableOutput(std::unique_ptr<AudioOutput> device);
    ~SwitchableOutput() override;

    /// The format of the device it was made with, which every one it is
    /// switched to has.
    const AudioFormat& format() const noexcept override { return format_; }

    Result<void> start(Pull pull, EndHandler ended) override;
    void stop() override;

    /// Play on `device` from now on: started at once if this is playing.
    /// Fails with InvalidArgument for a device of another format, and with
    /// the device's failure if it cannot start — the one held before then
    /// playing on.
    Result<void> switch_to(std::unique_ptr<AudioOutput> device);

private:
    const AudioFormat format_;
    std::mutex mutex_;
    std::unique_ptr<AudioOutput> device_;
    /// While started: what the device is started with.
    Pull pull_;
    EndHandler ended_;
    bool started_ = false;
};

}  // namespace ratcast
