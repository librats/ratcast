#include "ratcast/engine/audio_mixer.h"

#include <algorithm>
#include <utility>

namespace ratcast {

Result<void> AudioMixer::Input::start(Pull pull, EndHandler ended) {
    if (pull_) return fail(Errc::InvalidArgument);
    pull_ = std::move(pull);
    ended_ = std::move(ended);
    if (Result<void> started = mixer_.start(*this); !started) {
        pull_ = {};
        ended_ = {};
        return started;
    }
    return {};
}

void AudioMixer::Input::stop() {
    if (!pull_) return;
    mixer_.stop(*this);
    pull_ = {};
    ended_ = {};
}

void AudioMixer::Input::set_volume(float volume) noexcept {
    volume_.store(volume > 0.0f ? volume : 0.0f, std::memory_order_relaxed);  // NaN: 0
}

AudioMixer::~AudioMixer() {
    std::lock_guard lifecycle(lifecycle_);
    if (running_) output_.stop();
}

std::unique_ptr<AudioMixer::Input> AudioMixer::input() { return std::make_unique<Input>(*this); }

Result<void> AudioMixer::start(Input& input) {
    std::lock_guard lifecycle(lifecycle_);
    {
        std::lock_guard lock(mutex_);
        inputs_.push_back(&input);
    }
    if (running_) return {};
    Result<void> started =
        output_.start([this](std::span<float> out, Timestamp play_time) { pull(out, play_time); },
                      [this](Result<void> outcome) { ended(outcome); });
    if (!started) {
        std::lock_guard lock(mutex_);
        std::erase(inputs_, &input);
        return started;
    }
    running_ = true;
    return {};
}

void AudioMixer::stop(Input& input) {
    std::lock_guard lifecycle(lifecycle_);
    bool last = false;
    {
        // A pull holds the lock: once it is ours, the input is not in one.
        std::lock_guard lock(mutex_);
        std::erase(inputs_, &input);
        last = inputs_.empty();
    }
    // Not under mutex_: the device's thread may be waiting for it.
    if (last && running_) {
        output_.stop();
        running_ = false;
    }
}

void AudioMixer::pull(std::span<float> out, Timestamp play_time) {
    std::lock_guard lock(mutex_);
    std::ranges::fill(out, 0.0f);
    if (inputs_.size() == 1 && inputs_.front()->volume() == 1.0f) {
        inputs_.front()->pull_(out, play_time);
        return;
    }
    scratch_.resize(std::max(scratch_.size(), out.size()));
    const std::span<float> share(scratch_.data(), out.size());
    for (Input* input : inputs_) {
        std::ranges::fill(share, 0.0f);
        input->pull_(share, play_time);
        const float volume = input->volume();
        for (size_t i = 0; i < out.size(); ++i) out[i] += share[i] * volume;
    }
    for (float& sample : out) sample = std::clamp(sample, -1.0f, 1.0f);
}

void AudioMixer::ended(Result<void> outcome) {
    std::vector<AudioOutput::EndHandler> handlers;
    {
        std::lock_guard lock(mutex_);
        for (Input* input : inputs_) {
            if (input->ended_) handlers.push_back(input->ended_);
        }
    }
    for (const auto& handler : handlers) handler(outcome);
}

}  // namespace ratcast
