// Sound sources: the tone, whose samples follow on wherever a block is cut and
// whose blocks come in real time stamped as a device's are; the clock that
// stamps a real device's blocks; and what opening a device refuses, here or
// not. The devices themselves are tested in linux/test_pipewire_audio.cpp.

#include "ratcast/engine/tone_source.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "audio/sample_clock.h"
#include "ratcast/engine/audio_device.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

const Timestamp kStart{1s};

TEST(ToneTest, FollowsOnWhereverABlockIsCut) {
    const ToneConfig config{{48'000, 2}, 440.0f, 0.5f};
    const AudioFrame whole = draw_tone(config, 1000, 1000, kStart);
    const AudioFrame first = draw_tone(config, 1000, 400, kStart);
    const AudioFrame rest = draw_tone(config, 1400, 600, kStart);
    for (size_t i = 0; i < 2000; ++i) {
        const float expected = whole.samples()[i];
        const float got = i < 800 ? first.samples()[i] : rest.samples()[i - 800];
        ASSERT_EQ(got, expected) << "sample " << i;
    }
    EXPECT_EQ(whole.samples()[0], whole.samples()[1]);  // every channel the same
}

TEST(ToneTest, IsATone) {
    const AudioFrame second = draw_tone({{48'000, 1}, 440.0f, 0.5f}, 0, 48'000, kStart);
    size_t crossings = 0;
    float peak = 0;
    for (size_t i = 1; i < second.length(); ++i) {
        if ((second.samples()[i - 1] < 0) != (second.samples()[i] < 0)) ++crossings;
        peak = std::max(peak, std::abs(second.samples()[i]));
    }
    EXPECT_NEAR(double(crossings), 2 * 440, 2);
    EXPECT_NEAR(peak, 0.5f, 0.001f);
}

TEST(ToneSourceTest, HandsOutBlocksInRealTimeThatFollowOnFromTheFirst) {
    ManualClock clock(kStart);  // stamps come from the first block's time on
    ToneSource source({.block = 10ms}, clock);
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<AudioFrame> blocks;
    const auto began = std::chrono::steady_clock::now();
    ASSERT_TRUE(source.start(
        [&](AudioFrame block) {
            std::lock_guard lock(mutex);
            blocks.push_back(std::move(block));
            changed.notify_all();
        },
        {}));
    {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, 5s, [&] { return blocks.size() >= 20; });
    }
    const auto took = std::chrono::steady_clock::now() - began;
    source.stop();

    ASSERT_GE(blocks.size(), 20u);
    EXPECT_GE(took, 195ms);  // 20 blocks of 10 ms, none early
    for (size_t i = 0; i < blocks.size(); ++i) {
        EXPECT_EQ(blocks[i].length(), 480u);
        EXPECT_EQ(blocks[i].capture_time(), kStart + 10ms * int(i));
    }
    EXPECT_EQ(blocks[3].samples()[0], draw_tone({}, 3 * 480, 1, kStart).samples()[0]);

    // And again, from the start.
    EXPECT_TRUE(source.start([](AudioFrame) {}, {}));
}

TEST(SampleClockTest, RunsOnFromBlockToBlockThroughLateWakeUps) {
    SampleClock clock(48'000);
    // 10 ms blocks, each noticed up to 3 ms late.
    Timestamp due = kStart + 10ms;
    Timestamp previous = clock.stamp(480, due);
    EXPECT_EQ(previous, kStart);
    for (int i = 1; i < 100; ++i) {
        due += 10ms;
        const Duration late = Duration{(i * 7919) % 3000};
        const Timestamp stamp = clock.stamp(480, due + late);
        // Even, where arrival jitters by 3 ms — and late by no more than that,
        // which is as close as arrival can tell.
        EXPECT_LE(std::chrono::abs(stamp - previous - 10ms), 200us) << "block " << i;
        EXPECT_LE(std::chrono::abs(stamp - (kStart + 10ms * i)), 3ms) << "block " << i;
        previous = stamp;
    }
}

TEST(SampleClockTest, FollowsADeviceThatDriftsAndStartsAfreshAfterAStall) {
    SampleClock clock(48'000);
    // The device runs 1% fast: its 10 ms blocks come every 9.9 ms.
    Timestamp now = kStart;
    Timestamp stamp{};
    for (int i = 0; i < 500; ++i) {
        now += 9'900us;
        stamp = clock.stamp(480, now);
    }
    EXPECT_LE(std::chrono::abs(stamp - (now - 10ms)), 2ms);  // kept to the system clock

    // A stall of a second: the next block starts where it arrived.
    now += 1s;
    EXPECT_EQ(clock.stamp(480, now), now - 10ms);
}

TEST(AudioDeviceTest, RefusesAFormatWithoutChannelsOrRateAndALatencyOutOfRange) {
    const auto refused = [](auto result) {
        return !result && result.error() == Errc::InvalidArgument;
    };
    EXPECT_TRUE(refused(open_audio_capture({.format = {48'000, 0}})));
    EXPECT_TRUE(refused(open_audio_capture({.format = {0, 1}})));
    EXPECT_TRUE(refused(open_audio_capture({.latency = 0ms})));
    EXPECT_TRUE(refused(open_audio_output({.latency = 101ms})));
    EXPECT_TRUE(refused(open_audio_output({.format = {48'000, 0}})));
}

}  // namespace
