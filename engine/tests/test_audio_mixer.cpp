// AudioMixer over an output the test pulls by hand: inputs added up and
// clamped, the device running only while an input does, an input stopped
// never pulled again, and the device's end reaching every input.

#include "ratcast/engine/audio_mixer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

/// A device the test drives: pull() as its thread would.
class HandOutput final : public AudioOutput {
public:
    const AudioFormat& format() const noexcept override { return format_; }
    Result<void> start(Pull pull, EndHandler ended) override {
        std::lock_guard lock(mutex_);
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
        pull_(out, Timestamp{1s});
        return out;
    }
    void end() {
        EndHandler ended;
        {
            std::lock_guard lock(mutex_);
            ended = ended_;
        }
        ended(fail(Errc::Unavailable));
    }

    int starts = 0;

private:
    const AudioFormat format_{48'000, 2};
    std::mutex mutex_;
    AudioOutput::Pull pull_;
    AudioOutput::EndHandler ended_;
};

/// A player's pull: every sample `value`.
AudioOutput::Pull constant(float value) {
    return [value](std::span<float> out, Timestamp) { std::ranges::fill(out, value); };
}

TEST(AudioMixer, AddsItsInputsUp) {
    HandOutput device;
    AudioMixer mixer(device);
    auto music = mixer.input();
    auto voice = mixer.input();
    EXPECT_EQ(music->format(), device.format());

    ASSERT_TRUE(music->start(constant(0.25f), {}));
    EXPECT_EQ(device.pull(), std::vector<float>(8, 0.25f)) << "one alone, as it is";
    ASSERT_TRUE(voice->start(constant(0.5f), {}));
    EXPECT_EQ(device.pull(), std::vector<float>(8, 0.75f));
    music->stop();
    EXPECT_EQ(device.pull(), std::vector<float>(8, 0.5f));
    EXPECT_EQ(device.starts, 1) << "the device ran throughout";
}

TEST(AudioMixer, ClampsToFullScale) {
    HandOutput device;
    AudioMixer mixer(device);
    auto loud = mixer.input();
    auto louder = mixer.input();
    ASSERT_TRUE(loud->start(constant(0.8f), {}));
    ASSERT_TRUE(louder->start(constant(0.9f), {}));
    EXPECT_EQ(device.pull(), std::vector<float>(8, 1.0f));
    loud->stop();
    louder->stop();
    ASSERT_TRUE(loud->start(constant(-0.8f), {}));
    ASSERT_TRUE(louder->start(constant(-0.9f), {}));
    EXPECT_EQ(device.pull(), std::vector<float>(8, -1.0f));
}

TEST(AudioMixer, PlaysEachInputAtItsVolume) {
    HandOutput device;
    AudioMixer mixer(device);
    auto music = mixer.input();
    auto voice = mixer.input();
    EXPECT_EQ(music->volume(), 1.0f);

    ASSERT_TRUE(music->start(constant(0.5f), {}));
    music->set_volume(0.5f);
    EXPECT_EQ(device.pull(), std::vector<float>(8, 0.25f)) << "one alone, at its volume";
    music->set_volume(4.0f);
    EXPECT_EQ(device.pull(), std::vector<float>(8, 1.0f)) << "louder, clamped";

    int voice_pulls = 0;
    ASSERT_TRUE(voice->start(
        [&](std::span<float> out, Timestamp) {
            ++voice_pulls;
            std::ranges::fill(out, 0.25f);
        },
        {}));
    music->set_volume(0.5f);
    voice->set_volume(0.0f);
    EXPECT_EQ(device.pull(), std::vector<float>(8, 0.25f));
    EXPECT_EQ(voice_pulls, 1) << "silent, still pulled: it keeps its time";
    voice->set_volume(2.0f);
    EXPECT_EQ(device.pull(), std::vector<float>(8, 0.75f));

    for (const float none : {-1.0f, std::numeric_limits<float>::quiet_NaN()}) {
        voice->set_volume(none);
        EXPECT_EQ(voice->volume(), 0.0f);
    }
}

TEST(AudioMixer, TheDeviceRunsWhileAnInputDoes) {
    HandOutput device;
    AudioMixer mixer(device);
    auto input = mixer.input();
    EXPECT_FALSE(device.running());
    ASSERT_TRUE(input->start(constant(0.1f), {}));
    EXPECT_TRUE(device.running());
    EXPECT_FALSE(input->start(constant(0.1f), {})) << "started already";
    input->stop();
    EXPECT_FALSE(device.running());
    ASSERT_TRUE(input->start(constant(0.1f), {}));
    EXPECT_EQ(device.starts, 2);
    input.reset();  // stops it
    EXPECT_FALSE(device.running());
}

TEST(AudioMixer, AnInputStoppedIsNotPulledAgain) {
    HandOutput device;
    AudioMixer mixer(device);
    auto keeps = mixer.input();
    auto goes = mixer.input();
    ASSERT_TRUE(keeps->start(constant(0.1f), {}));
    std::atomic<bool> stopped{false};
    std::atomic<int> pulled_after{0};
    ASSERT_TRUE(goes->start(
        [&](std::span<float> out, Timestamp) {
            if (stopped) ++pulled_after;
            std::ranges::fill(out, 0.1f);
        },
        {}));
    std::atomic<bool> done{false};
    std::thread device_thread([&] {
        while (!done) device.pull(480);
    });
    std::this_thread::sleep_for(5ms);
    goes->stop();
    stopped = true;
    std::this_thread::sleep_for(5ms);
    done = true;
    device_thread.join();
    EXPECT_EQ(pulled_after, 0);
}

TEST(AudioMixer, TheDevicesEndReachesEveryInput) {
    HandOutput device;
    AudioMixer mixer(device);
    auto first = mixer.input();
    auto second = mixer.input();
    std::atomic<int> told{0};
    const auto count = [&](Result<void> outcome) {
        EXPECT_FALSE(outcome);
        ++told;
    };
    ASSERT_TRUE(first->start(constant(0.1f), count));
    ASSERT_TRUE(second->start(constant(0.1f), count));
    device.end();
    EXPECT_EQ(told, 2);
}

}  // namespace
