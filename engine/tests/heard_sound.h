#pragma once

/**
 * @file heard_sound.h
 * @brief What the sound device tests hear, and how they measure it: a
 * capture's blocks gathered for the test thread to wait on, a tone's
 * amplitude at one pitch, and a level.
 */

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/engine/audio_frame.h"
#include "ratcast/engine/audio_source.h"

namespace ratcast::test {

inline constexpr Duration kSoundPatience = std::chrono::seconds(5);

/// What a capture handed out, for the test thread to wait on.
class Heard {
public:
    AudioSource::Sink sink() {
        return [this](AudioFrame block) {
            std::lock_guard lock(mutex_);
            blocks_.push_back(std::move(block));
            changed_.notify_all();
        };
    }
    AudioSource::EndHandler ended() {
        return [this](Result<void> outcome) {
            std::lock_guard lock(mutex_);
            outcome_ = outcome;
            changed_.notify_all();
        };
    }

    /// Blocks until they hold `duration` of sound with the tone in it, or
    /// nothing after kSoundPatience. Silence before the tone (the link being
    /// made) is left out.
    std::vector<AudioFrame> wait_for_tone(Duration duration) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, kSoundPatience, [&] { return tone_heard() >= duration; });
        std::vector<AudioFrame> tone;
        for (const AudioFrame& block : blocks_) {
            if (!tone.empty() || loud(block)) tone.push_back(block);
        }
        return tone;
    }

    /// Blocks until `more` sound than so far has come, or kSoundPatience has
    /// passed.
    std::vector<AudioFrame> wait_for_more(Duration more) {
        std::unique_lock lock(mutex_);
        const Duration enough = heard() + more;
        changed_.wait_for(lock, kSoundPatience, [&] { return heard() >= enough; });
        return blocks_;
    }

    /// Blocks once `done(blocks)`, or kSoundPatience has passed.
    template <typename Done>
    std::vector<AudioFrame> wait_until(Done done) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, kSoundPatience, [&] { return done(blocks_); });
        return blocks_;
    }

    std::optional<Result<void>> wait_for_end() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, kSoundPatience, [&] { return outcome_.has_value(); });
        return outcome_;
    }

private:
    Duration heard() const {
        Duration heard{};
        for (const AudioFrame& block : blocks_) heard += block.duration();
        return heard;
    }

    static bool loud(const AudioFrame& block) {
        for (float s : block.samples()) {
            if (std::abs(s) > 0.1f) return true;
        }
        return false;
    }

    Duration tone_heard() const {
        Duration heard{};
        bool started = false;
        for (const AudioFrame& block : blocks_) {
            started = started || loud(block);
            if (started) heard += block.duration();
        }
        return heard;
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<AudioFrame> blocks_;
    std::optional<Result<void>> outcome_;
};

/// The amplitude of a tone at `frequency` in one channel of the last 200 ms
/// of `blocks`, at 48 kHz (Goertzel's algorithm; a whole number of cycles of
/// a multiple of 5 Hz fits in 200 ms, so a tone at one is measured whole).
inline double amplitude(const std::vector<AudioFrame>& blocks, double frequency,
                        size_t channel = 0) {
    std::vector<float> samples;
    for (const AudioFrame& block : blocks) {
        const size_t channels = block.format().channels;
        for (size_t i = channel; i < block.samples().size(); i += channels)
            samples.push_back(block.samples()[i]);
    }
    const size_t length = 48'000 / 5;
    if (samples.size() < length) return 0;
    const double coefficient = 2 * std::cos(2 * std::numbers::pi * frequency / 48'000);
    double s1 = 0;
    double s2 = 0;
    for (float x : std::span(samples).last(length)) {
        const double s0 = x + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2 * std::sqrt(s1 * s1 + s2 * s2 - coefficient * s1 * s2) / double(length);
}

inline float rms(const std::vector<AudioFrame>& blocks) {
    double sum = 0;
    size_t count = 0;
    for (const AudioFrame& block : blocks) {
        for (float s : block.samples()) sum += double(s) * s;
        count += block.samples().size();
    }
    return count ? float(std::sqrt(sum / count)) : 0.0f;
}

}  // namespace ratcast::test
