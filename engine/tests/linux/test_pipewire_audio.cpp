// pipewire::AudioPlayback and pipewire::AudioCapture on the user's PipeWire
// daemon, each the other's test: a playback of the tone, and a capture of that
// playback's node, which hears the tone in blocks that follow on in time — and
// ends when the node goes, or when there never was one; and a capture of the
// default output, which hears the playback too; and pipewire::
// ApplicationsCapture, which hears the playback unless it leaves this process
// out, and goes on without it. Skipped where there is no PipeWire (CI).

#include "linux/pipewire_applications.h"
#include "linux/pipewire_audio.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
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

class PipeWireAudioTest : public ::testing::Test {
protected:
    void SetUp() override {
        playback_ = std::make_unique<pipewire::AudioPlayback>(AudioOutputOptions{kMono});
        if (!play()) GTEST_SKIP() << "no PipeWire daemon here";
    }

    /// Start the playback of the tone at `pitch`, and wait for its node.
    bool play(float pitch = kTone.frequency) {
        tone_.frequency = pitch;
        const Result<void> started = playback_->start(
            [this](std::span<float> out, Timestamp play_time) {
                const size_t length = out.size();
                const AudioFrame tone = draw_tone(tone_, played_, length, play_time);
                std::copy(tone.samples().begin(), tone.samples().end(), out.begin());
                played_ += length;
                if (play_time < SteadyClock::instance().now()) early_ = true;
            },
            {});
        if (!started) return false;
        const auto deadline = std::chrono::steady_clock::now() + kSoundPatience;
        while (playback_->node_id() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        return playback_->node_id() != 0;
    }

    ToneConfig tone_ = kTone;
    std::unique_ptr<pipewire::AudioPlayback> playback_;
    uint64_t played_ = 0;  ///< on the playback's thread
    std::atomic<bool> early_{false};
};

}  // namespace

TEST_F(PipeWireAudioTest, ACaptureOfAPlaybackHearsItInBlocksThatFollowOn) {
    pipewire::AudioCapture capture({.format = kMono, .node = playback_->node_id()});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));

    const std::vector<AudioFrame> tone = heard.wait_for_tone(500ms);
    ASSERT_FALSE(tone.empty()) << "nothing heard";
    EXPECT_NEAR(rms(tone), 0.25f / std::sqrt(2.0f), 0.02f);
    for (size_t i = 1; i < tone.size(); ++i) {
        EXPECT_EQ(tone[i].format(), kMono);
        const Timestamp follows_on = tone[i - 1].capture_time() + tone[i - 1].duration();
        EXPECT_LE(std::chrono::abs(tone[i].capture_time() - follows_on), 2ms) << "block " << i;
    }
    // Captured as it was played: the times are now's, not the future's.
    EXPECT_LE(tone.back().capture_time(), SteadyClock::instance().now());
    EXPECT_FALSE(early_) << "a play time before the pull";
}

TEST_F(PipeWireAudioTest, ACaptureEndsWhenItsNodeGoes) {
    pipewire::AudioCapture capture({.format = kMono, .node = playback_->node_id()});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    ASSERT_FALSE(heard.wait_for_tone(50ms).empty());

    playback_->stop();
    const std::optional<Result<void>> outcome = heard.wait_for_end();
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(*outcome);
}

TEST_F(PipeWireAudioTest, ACaptureOfANodeThatIsNotThereEnds) {
    pipewire::AudioCapture capture({.format = kMono, .node = 999'999});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    const std::optional<Result<void>> outcome = heard.wait_for_end();
    ASSERT_TRUE(outcome);
    ASSERT_FALSE(*outcome);
    EXPECT_EQ(outcome->error(), Errc::Unavailable);
}

TEST_F(PipeWireAudioTest, StopsAndStartsAgain) {
    pipewire::AudioCapture capture({.format = kMono, .node = playback_->node_id()});
    for (int round = 0; round < 2; ++round) {
        Heard heard;
        ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
        EXPECT_FALSE(capture.start(heard.sink(), heard.ended()));  // already started
        EXPECT_FALSE(heard.wait_for_tone(50ms).empty()) << "round " << round;
        capture.stop();
    }
}

TEST_F(PipeWireAudioTest, ACaptureOfSystemSoundHearsWhatIsPlayed) {
    // The playback went to the default output, whose monitor this captures.
    pipewire::AudioCapture capture({.format = kMono, .source = AudioCaptureSource::SystemSound});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    const std::vector<AudioFrame> tone = heard.wait_for_tone(200ms);
    if (tone.empty()) GTEST_SKIP() << "no default output here to listen to";
    EXPECT_GT(rms(tone), 0.05f);  // at whatever volume the output is set to
}

/// Other processes may be playing too — tests run side by side — so each
/// test here plays at a pitch of its own, and measures the sound at it.
class PipeWireApplicationsTest : public PipeWireAudioTest {
protected:
    void SetUp() override {
        playback_ = std::make_unique<pipewire::AudioPlayback>(AudioOutputOptions{kMono});
    }
};

TEST_F(PipeWireApplicationsTest, AreHeardOnBothSidesIfThisProcessIsNotLeftOut) {
    // This process plays the tone, in mono; leaving out another (init, which
    // plays nothing), it is heard, in both channels.
    constexpr double kPitch = 1250;
    if (!play(kPitch)) GTEST_SKIP() << "no PipeWire daemon here";
    pipewire::ApplicationsCapture capture(
        {.format = {48'000, 2}, .source = AudioCaptureSource::Applications, .excluded_process = 1});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    // Once it is there, 200 ms of it whole: what is measured.
    heard.wait_until([&](const auto& blocks) { return amplitude(blocks, kPitch, 0) > 0.1; });
    const std::vector<AudioFrame> blocks = heard.wait_for_more(300ms);
    EXPECT_NEAR(amplitude(blocks, kPitch, 0), 0.25, 0.03) << "left";
    EXPECT_NEAR(amplitude(blocks, kPitch, 1), 0.25, 0.03) << "right";
    for (size_t i = 1; i < blocks.size(); ++i) {
        const Timestamp follows_on = blocks[i - 1].capture_time() + blocks[i - 1].duration();
        EXPECT_LE(std::chrono::abs(blocks[i].capture_time() - follows_on), 2ms) << "block " << i;
    }
}

TEST_F(PipeWireApplicationsTest, LeaveThisProcessOutAndRunOnWithoutIt) {
    constexpr double kPitch = 1500;
    if (!play(kPitch)) GTEST_SKIP() << "no PipeWire daemon here";
    pipewire::ApplicationsCapture capture(
        {.format = kMono, .source = AudioCaptureSource::Applications});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    const std::vector<AudioFrame> blocks = heard.wait_for_more(500ms);
    Duration heard_for{};
    for (const AudioFrame& block : blocks) heard_for += block.duration();
    ASSERT_GE(heard_for, 500ms) << "the sound goes on";
    EXPECT_LT(amplitude(blocks, kPitch), 0.01) << "this process's own tone heard";
}

TEST_F(PipeWireApplicationsTest, OneThatStartsLaterIsHeard) {
    constexpr double kPitch = 1750;
    pipewire::ApplicationsCapture capture(
        {.format = kMono, .source = AudioCaptureSource::Applications, .excluded_process = 1});
    Heard heard;
    if (!capture.start(heard.sink(), heard.ended())) GTEST_SKIP() << "no PipeWire daemon here";
    ASSERT_FALSE(heard.wait_for_more(100ms).empty());
    ASSERT_TRUE(play(kPitch));
    heard.wait_until([&](const auto& blocks) { return amplitude(blocks, kPitch) > 0.1; });
    const std::vector<AudioFrame> blocks = heard.wait_for_more(300ms);
    EXPECT_NEAR(amplitude(blocks, kPitch), 0.25, 0.03) << "the mean of both sides, not their sum";
}
