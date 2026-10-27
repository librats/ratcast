// core_audio::AudioPlayback and core_audio::AudioCapture on this computer's
// sound, each the other's test: a playback of the tone, and a capture of this
// process's sound, which hears the tone in blocks that follow on in time; a
// capture of what the outputs play, which hears the playback too; a capture
// of the applications, which runs on without this process's tone when it
// leaves it out; one of a process that is not there, which ends; the devices
// listed, one chosen played to, and a loopback device (BlackHole, which CI
// installs) heard as a microphone playing back what is played to it.
// Skipped where there is no sound output; a capture that hears only silence
// has not been given the user's leave, and what it would have heard is
// skipped too.

#include "macos/core_audio.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "heard_sound.h"
#include "ratcast/engine/tone_source.h"

using namespace ratcast;
using namespace ratcast::test;
using namespace std::chrono_literals;

namespace {

constexpr AudioFormat kMono{48'000, 1};
const ToneConfig kTone{kMono, 440.0f, 0.25f};

uint32_t this_process() { return uint32_t(getpid()); }

void expect_follow_on(const std::vector<AudioFrame>& blocks) {
    for (size_t i = 1; i < blocks.size(); ++i) {
        EXPECT_EQ(blocks[i].format(), blocks[0].format());
        const Timestamp follows_on = blocks[i - 1].capture_time() + blocks[i - 1].duration();
        EXPECT_LE(std::chrono::abs(blocks[i].capture_time() - follows_on), 2ms) << "block " << i;
    }
}

Duration length_of(const std::vector<AudioFrame>& blocks) {
    Duration total{};
    for (const AudioFrame& block : blocks) total += block.duration();
    return total;
}

/// A playback of the tone at `pitch`, to the default output or a device.
class Tone {
public:
    explicit Tone(float pitch = kTone.frequency, std::string device = {})
        : playback_({.format = kMono, .device = std::move(device)}) {
        tone_.frequency = pitch;
    }

    bool play() {
        return bool(playback_.start(
            [this](std::span<float> out, Timestamp play_time) {
                const AudioFrame tone = draw_tone(tone_, played_, out.size(), play_time);
                std::copy(tone.samples().begin(), tone.samples().end(), out.begin());
                played_ += out.size();
                if (play_time < SteadyClock::instance().now()) early_ = true;
            },
            {}));
    }
    void stop() { playback_.stop(); }
    bool early() const { return early_; }

private:
    ToneConfig tone_ = kTone;
    core_audio::AudioPlayback playback_;
    uint64_t played_ = 0;  ///< on the playback's thread
    std::atomic<bool> early_{false};
};

class CoreAudioTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!tone_.play()) GTEST_SKIP() << "no sound output here";
    }

    Tone tone_;
};

}  // namespace

TEST_F(CoreAudioTest, ACaptureOfAProcessHearsItInBlocksThatFollowOn) {
    core_audio::AudioCapture capture({.format = kMono, .node = this_process()});
    Heard heard;
    if (!capture.start(heard.sink(), heard.ended())) GTEST_SKIP() << "no process taps here";

    const std::vector<AudioFrame> tone = heard.wait_for_tone(500ms);
    if (tone.empty()) GTEST_SKIP() << "no leave to hear the applications' sound";
    EXPECT_NEAR(rms(tone), 0.25f / std::sqrt(2.0f), 0.02f);
    expect_follow_on(tone);
    // Captured as it was played: the times are now's, not the future's.
    EXPECT_LE(tone.back().capture_time(), SteadyClock::instance().now());
    EXPECT_FALSE(tone_.early()) << "a play time before the pull";
}

TEST_F(CoreAudioTest, ACaptureOfAProcessThatIsNotThereEnds) {
    core_audio::AudioCapture capture({.format = kMono, .node = 0x7FFF'FFF0});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    const std::optional<Result<void>> outcome = heard.wait_for_end();
    ASSERT_TRUE(outcome);
    ASSERT_FALSE(*outcome);
    EXPECT_EQ(outcome->error(), Errc::Unavailable);
}

TEST_F(CoreAudioTest, ACaptureStopsAndStartsAgain) {
    core_audio::AudioCapture capture({.format = kMono, .source = AudioCaptureSource::Applications});
    for (int round = 0; round < 2; ++round) {
        Heard heard;
        if (!capture.start(heard.sink(), heard.ended())) GTEST_SKIP() << "no process taps here";
        EXPECT_FALSE(capture.start(heard.sink(), heard.ended()));  // already started
        EXPECT_FALSE(heard.wait_for_more(50ms).empty()) << "round " << round;
        capture.stop();
    }
}

TEST_F(CoreAudioTest, APlaybackStopsAndStartsAgain) {
    tone_.stop();
    EXPECT_TRUE(tone_.play());
    EXPECT_FALSE(tone_.play()) << "already started";
}

TEST_F(CoreAudioTest, ACaptureOfSystemSoundHearsWhatIsPlayed) {
    core_audio::AudioCapture capture({.format = kMono, .source = AudioCaptureSource::SystemSound});
    Heard heard;
    if (!capture.start(heard.sink(), heard.ended())) GTEST_SKIP() << "no process taps here";
    const std::vector<AudioFrame> tone = heard.wait_for_tone(200ms);
    if (tone.empty()) GTEST_SKIP() << "no leave to hear the computer's sound";
    EXPECT_GT(rms(tone), 0.05f);
    expect_follow_on(tone);
}

/// Other processes may be playing too — tests run side by side — so each
/// test here plays at a pitch of its own, and measures the sound at it.
TEST(CoreAudioApplicationsTest, LeaveThisProcessOutAndRunOnWithoutIt) {
    constexpr float kPitch = 1500;
    Tone tone(kPitch);
    if (!tone.play()) GTEST_SKIP() << "no sound output here";
    core_audio::AudioCapture capture({.format = kMono, .source = AudioCaptureSource::Applications});
    Heard heard;
    if (!capture.start(heard.sink(), heard.ended())) GTEST_SKIP() << "no process taps here";
    const std::vector<AudioFrame> blocks = heard.wait_for_more(500ms);
    ASSERT_GE(length_of(blocks), 500ms) << "the sound goes on";
    EXPECT_LT(amplitude(blocks, kPitch), 0.01) << "this process's own tone heard";
    expect_follow_on(blocks);
}

TEST(CoreAudioApplicationsTest, AreHeardOnBothSidesInStereo) {
    constexpr float kPitch = 1250;
    Tone tone(kPitch);
    if (!tone.play()) GTEST_SKIP() << "no sound output here";
    // The applications but another process, which plays nothing: this one's
    // tone, in mono, is heard in both channels.
    core_audio::AudioCapture capture(
        {.format = {48'000, 2}, .source = AudioCaptureSource::Applications, .excluded_process = 1});
    Heard heard;
    if (!capture.start(heard.sink(), heard.ended())) GTEST_SKIP() << "no process taps here";
    const auto loud = [&](const auto& blocks) { return amplitude(blocks, kPitch, 0) > 0.1; };
    if (!loud(heard.wait_until(loud))) GTEST_SKIP() << "no leave to hear the applications' sound";
    const std::vector<AudioFrame> blocks = heard.wait_for_more(300ms);
    EXPECT_NEAR(amplitude(blocks, kPitch, 0), 0.25, 0.03) << "left";
    EXPECT_NEAR(amplitude(blocks, kPitch, 1), 0.25, 0.03) << "right";
    expect_follow_on(blocks);
}

TEST(CoreAudioDevicesTest, AreListedByIdAndName) {
    const Result<std::vector<AudioDeviceInfo>> outputs = core_audio::list_devices(false);
    const Result<std::vector<AudioDeviceInfo>> inputs = core_audio::list_devices(true);
    ASSERT_TRUE(outputs);
    ASSERT_TRUE(inputs);
    for (const std::vector<AudioDeviceInfo>* devices : {&*outputs, &*inputs}) {
        for (const AudioDeviceInfo& device : *devices) {
            EXPECT_FALSE(device.id.empty());
            EXPECT_FALSE(device.name.empty());
        }
    }
}

TEST(CoreAudioDevicesTest, ALoopbackDeviceHearsWhatIsPlayedToIt) {
    // A device that is an output and an input both, as BlackHole is: what is
    // played to it is what it hears.
    const Result<std::vector<AudioDeviceInfo>> outputs = core_audio::list_devices(false);
    const Result<std::vector<AudioDeviceInfo>> inputs = core_audio::list_devices(true);
    ASSERT_TRUE(outputs);
    ASSERT_TRUE(inputs);
    std::string loopback;
    for (const AudioDeviceInfo& output : *outputs) {
        if (output.name.find("BlackHole") == std::string::npos) continue;
        for (const AudioDeviceInfo& input : *inputs)
            if (input.id == output.id) loopback = output.id;
    }
    if (loopback.empty()) GTEST_SKIP() << "no loopback device here";

    Tone tone(kTone.frequency, loopback);
    ASSERT_TRUE(tone.play());
    core_audio::AudioCapture capture({.format = kMono, .device = loopback});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    const std::vector<AudioFrame> heard_tone = heard.wait_for_tone(300ms);
    if (heard_tone.empty()) GTEST_SKIP() << "no leave to use the microphone";
    EXPECT_NEAR(rms(heard_tone), 0.25f / std::sqrt(2.0f), 0.03f);
    expect_follow_on(heard_tone);
    EXPECT_LE(heard_tone.back().capture_time(), SteadyClock::instance().now());
}

TEST(CoreAudioMicrophoneTest, HandsOutBlocksThatFollowOn) {
    core_audio::AudioCapture capture({.format = kMono});
    Heard heard;
    if (!capture.start(heard.sink(), heard.ended())) GTEST_SKIP() << "no microphone here";
    const std::vector<AudioFrame> blocks = heard.wait_for_more(300ms);
    EXPECT_GE(length_of(blocks), 300ms);
    expect_follow_on(blocks);
}

TEST(CoreAudioMicrophoneTest, OneChosenOrOneNotThereHandsOutBlocks) {
    const Result<std::vector<AudioDeviceInfo>> inputs = core_audio::list_devices(true);
    if (!inputs || inputs->empty()) GTEST_SKIP() << "no microphone here";
    for (const std::string& id : {inputs->front().id, std::string("ratcast-not-there")}) {
        core_audio::AudioCapture capture({.format = {44'100, 2}, .device = id});
        Heard heard;
        ASSERT_TRUE(capture.start(heard.sink(), heard.ended())) << id;
        const std::vector<AudioFrame> blocks = heard.wait_for_more(100ms);
        EXPECT_GE(length_of(blocks), 100ms) << id;
        expect_follow_on(blocks);
    }
}

TEST(CoreAudioPlaybackTest, ToADeviceChosenOrOneNotThere) {
    const Result<std::vector<AudioDeviceInfo>> outputs = core_audio::list_devices(false);
    if (!outputs || outputs->empty()) GTEST_SKIP() << "no sound output here";
    for (const std::string& id : {outputs->back().id, std::string("ratcast-not-there")}) {
        std::atomic<int> pulls{0};
        core_audio::AudioPlayback playback({.format = kMono, .device = id});
        ASSERT_TRUE(playback.start(
            [&](std::span<float> out, Timestamp) {
                std::fill(out.begin(), out.end(), 0.0f);
                ++pulls;
            },
            {}))
            << id;
        const Timestamp until = SteadyClock::instance().now() + 2s;
        while (pulls < 5 && SteadyClock::instance().now() < until)
            std::this_thread::sleep_for(10ms);
        EXPECT_GE(pulls, 5) << id;
        playback.stop();
    }
}
