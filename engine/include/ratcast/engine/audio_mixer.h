#pragma once

/**
 * @file audio_mixer.h
 * @brief Several players into one output, their sound added up.
 *
 * An AudioMixer stands over one output — a device — and hands out inputs,
 * each an AudioOutput of the device's format, which a player starts and
 * stops as it would the device. The device runs while any input does. Each
 * of its pulls asks every running input for the same span of time and adds
 * what they give, each as loud as its volume says, clamped to full scale; one
 * input alone at its own volume is pulled straight into the device's buffer.
 *
 * Once an input's stop() returns it is not pulled again. When the device
 * stops by itself, every running input hears it. Thread-safe.
 */

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/audio_device.h"

namespace ratcast {

class AudioMixer {
public:
    class Input;

    /// `output` must outlive the mixer, and the mixer its inputs.
    explicit AudioMixer(AudioOutput& output) noexcept : output_(output) {}
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    /// A new input, stopped, at its own volume.
    std::unique_ptr<Input> input();

private:
    Result<void> start(Input& input);
    void stop(Input& input);
    /// The device's pull: every running input's sound, added up.
    void pull(std::span<float> out, Timestamp play_time);
    void ended(Result<void> outcome);

    AudioOutput& output_;
    std::mutex lifecycle_;        ///< serialises starting and stopping, and the device's
    bool running_ = false;        ///< the device; under lifecycle_
    std::mutex mutex_;            ///< held by each pull
    std::vector<Input*> inputs_;  ///< running
    std::vector<float> scratch_;  ///< each input's share of a pull
};

class AudioMixer::Input final : public AudioOutput {
public:
    explicit Input(AudioMixer& mixer) noexcept : mixer_(mixer) {}
    ~Input() override { stop(); }

    const AudioFormat& format() const noexcept override { return mixer_.output_.format(); }
    Result<void> start(Pull pull, EndHandler ended) override;
    void stop() override;

    /// How loud what it is given plays: 1 as it is, 0 silent, 2 twice the
    /// amplitude (+6 dB); below 0, or not a number, 0. From any thread, from
    /// the next pull on; it is still pulled when silent.
    void set_volume(float volume) noexcept;
    float volume() const noexcept { return volume_.load(std::memory_order_relaxed); }

private:
    friend class AudioMixer;

    AudioMixer& mixer_;
    Pull pull_;
    EndHandler ended_;
    std::atomic<float> volume_{1.0f};
};

}  // namespace ratcast
