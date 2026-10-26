#pragma once

/**
 * @file hand_output.h
 * @brief An audio output the test pulls by hand, as a device's thread would.
 */

#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "ratcast/engine/audio_device.h"

namespace ratcast::test {

class HandOutput final : public AudioOutput {
public:
    explicit HandOutput(AudioFormat format = {48'000, 2}) : format_(format) {}

    const AudioFormat& format() const noexcept override { return format_; }
    Result<void> start(Pull pull, EndHandler ended) override {
        std::lock_guard lock(mutex_);
        if (refuse) return fail(Errc::Unavailable);
        if (pull_) return fail(Errc::InvalidArgument);
        pull_ = std::move(pull);
        ended_ = std::move(ended);
        ++starts;
        return {};
    }
    void stop() override {
        std::lock_guard lock(mutex_);
        pull_ = {};
        ended_ = {};
    }

    bool running() {
        std::lock_guard lock(mutex_);
        return static_cast<bool>(pull_);
    }
    /// One pull of `frames` sample times; empty if the device is stopped.
    std::vector<float> pull(size_t frames = 4) {
        std::lock_guard lock(mutex_);
        if (!pull_) return {};
        std::vector<float> out(frames * format_.channels, -9.0f);  // garbage to be overwritten
        pull_(out, Timestamp{std::chrono::seconds(1)});
        return out;
    }
    /// The device gone: its end handler told.
    void end() {
        EndHandler ended;
        {
            std::lock_guard lock(mutex_);
            ended = ended_;
        }
        ended(fail(Errc::Unavailable));
    }

    int starts = 0;
    bool refuse = false;  ///< start() fails, as a device that is not there

private:
    const AudioFormat format_;
    std::mutex mutex_;
    AudioOutput::Pull pull_;
    AudioOutput::EndHandler ended_;
};

}  // namespace ratcast::test
