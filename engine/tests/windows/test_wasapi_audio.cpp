// wasapi::AudioPlayback and wasapi::AudioCapture on this computer's sound,
// each the other's test: a playback of the tone, and a capture of this
// process's sound, which hears the tone in blocks that follow on in time —
// and ends when its process exits, or when there never was one; a capture of
// the default output, which hears the playback too; and a capture of the
// applications, which hears the playback unless it leaves this process out,
// and goes on without it. Skipped where there is no sound output (CI).

#include "windows/wasapi_audio.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "heard_sound.h"
#include "ratcast/engine/tone_source.h"

using namespace ratcast;
using namespace ratcast::test;
using namespace std::chrono_literals;

namespace {

constexpr AudioFormat kMono{48'000, 1};
const ToneConfig kTone{kMono, 440.0f, 0.25f};

uint32_t this_process() { return GetCurrentProcessId(); }

/// A process that plays nothing and waits to be ended: what a capture of a
/// process ends with, or leaves out in place of this one.
class Bystander {
public:
    Bystander() {
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        std::wstring command = L"ping.exe -n 60 127.0.0.1";
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &info_)) {
            CloseHandle(info_.hThread);
        } else {
            info_ = {};
        }
    }
    ~Bystander() {
        end();
        if (info_.hProcess) CloseHandle(info_.hProcess);
    }

    uint32_t id() const { return info_.dwProcessId; }
    void end() {
        if (!info_.hProcess) return;
        TerminateProcess(info_.hProcess, 0);
        WaitForSingleObject(info_.hProcess, 5000);
    }

private:
    PROCESS_INFORMATION info_{};
};

class WasapiAudioTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!play()) GTEST_SKIP() << "no sound output here";
    }

    /// Start the playback of the tone at `pitch`.
    bool play(float pitch = kTone.frequency) {
        tone_.frequency = pitch;
        return bool(playback_.start(
            [this](std::span<float> out, Timestamp play_time) {
                const size_t length = out.size();
                const AudioFrame tone = draw_tone(tone_, played_, length, play_time);
                std::copy(tone.samples().begin(), tone.samples().end(), out.begin());
                played_ += length;
                if (play_time < SteadyClock::instance().now()) early_ = true;
            },
            {}));
    }

    ToneConfig tone_ = kTone;
    wasapi::AudioPlayback playback_{AudioOutputOptions{kMono}};
    uint64_t played_ = 0;  ///< on the playback's thread
    std::atomic<bool> early_{false};
};

}  // namespace

TEST_F(WasapiAudioTest, ACaptureOfAProcessHearsItInBlocksThatFollowOn) {
    wasapi::AudioCapture capture({.format = kMono, .node = this_process()});
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

TEST_F(WasapiAudioTest, ACaptureEndsWhenItsProcessExits) {
    Bystander bystander;
    ASSERT_NE(bystander.id(), 0u);
    wasapi::AudioCapture capture({.format = kMono, .node = bystander.id()});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    ASSERT_FALSE(heard.wait_for_more(50ms).empty()) << "its silence, until it goes";

    bystander.end();
    const std::optional<Result<void>> outcome = heard.wait_for_end();
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(*outcome);
}

TEST_F(WasapiAudioTest, ACaptureOfAProcessThatIsNotThereEnds) {
    wasapi::AudioCapture capture({.format = kMono, .node = 0xFFFF'FFFC});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    const std::optional<Result<void>> outcome = heard.wait_for_end();
    ASSERT_TRUE(outcome);
    ASSERT_FALSE(*outcome);
    EXPECT_EQ(outcome->error(), Errc::Unavailable);
}

TEST_F(WasapiAudioTest, StopsAndStartsAgain) {
    wasapi::AudioCapture capture({.format = kMono, .node = this_process()});
    for (int round = 0; round < 2; ++round) {
        Heard heard;
        ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
        EXPECT_FALSE(capture.start(heard.sink(), heard.ended()));  // already started
        EXPECT_FALSE(heard.wait_for_tone(50ms).empty()) << "round " << round;
        capture.stop();
    }
}

TEST_F(WasapiAudioTest, APlaybackStopsAndStartsAgain) {
    playback_.stop();
    EXPECT_TRUE(play());
    EXPECT_FALSE(play()) << "already started";
}

TEST_F(WasapiAudioTest, ACaptureOfSystemSoundHearsWhatIsPlayed) {
    wasapi::AudioCapture capture({.format = kMono, .source = AudioCaptureSource::SystemSound});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    const std::vector<AudioFrame> tone = heard.wait_for_tone(200ms);
    if (tone.empty()) GTEST_SKIP() << "the default output is muted";
    EXPECT_GT(rms(tone), 0.05f);  // at whatever volume the output is set to
}

/// Other processes may be playing too — tests run side by side — so each
/// test here plays at a pitch of its own, and measures the sound at it.
class WasapiApplicationsTest : public WasapiAudioTest {
protected:
    void SetUp() override {}
};

TEST_F(WasapiApplicationsTest, AreHeardOnBothSidesIfThisProcessIsNotLeftOut) {
    // This process plays the tone, in mono; leaving out another, which plays
    // nothing, it is heard, in both channels.
    constexpr double kPitch = 1250;
    if (!play(kPitch)) GTEST_SKIP() << "no sound output here";
    Bystander bystander;
    ASSERT_NE(bystander.id(), 0u);
    wasapi::AudioCapture capture({.format = {48'000, 2},
                                  .source = AudioCaptureSource::Applications,
                                  .excluded_process = int(bystander.id())});
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

TEST_F(WasapiApplicationsTest, LeaveThisProcessOutAndRunOnWithoutIt) {
    constexpr double kPitch = 1500;
    if (!play(kPitch)) GTEST_SKIP() << "no sound output here";
    wasapi::AudioCapture capture({.format = kMono, .source = AudioCaptureSource::Applications});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    const std::vector<AudioFrame> blocks = heard.wait_for_more(500ms);
    Duration heard_for{};
    for (const AudioFrame& block : blocks) heard_for += block.duration();
    ASSERT_GE(heard_for, 500ms) << "the sound goes on";
    EXPECT_LT(amplitude(blocks, kPitch), 0.01) << "this process's own tone heard";
    for (size_t i = 1; i < blocks.size(); ++i) {
        const Timestamp follows_on = blocks[i - 1].capture_time() + blocks[i - 1].duration();
        EXPECT_LE(std::chrono::abs(blocks[i].capture_time() - follows_on), 2ms) << "block " << i;
    }
}

TEST_F(WasapiApplicationsTest, OneThatStartsLaterIsHeard) {
    constexpr double kPitch = 1750;
    Bystander bystander;
    ASSERT_NE(bystander.id(), 0u);
    wasapi::AudioCapture capture({.format = kMono,
                                  .source = AudioCaptureSource::Applications,
                                  .excluded_process = int(bystander.id())});
    Heard heard;
    if (!capture.start(heard.sink(), heard.ended())) GTEST_SKIP() << "no sound output here";
    ASSERT_FALSE(heard.wait_for_more(100ms).empty());
    if (!play(kPitch)) GTEST_SKIP() << "no sound output here";
    heard.wait_until([&](const auto& blocks) { return amplitude(blocks, kPitch) > 0.1; });
    const std::vector<AudioFrame> blocks = heard.wait_for_more(300ms);
    EXPECT_NEAR(amplitude(blocks, kPitch), 0.25, 0.03) << "the mean of both sides, not their sum";
}

TEST(WasapiMicrophoneTest, HandsOutBlocksThatFollowOn) {
    wasapi::AudioCapture capture({.format = kMono});
    Heard heard;
    if (!capture.start(heard.sink(), heard.ended())) GTEST_SKIP() << "no microphone here";
    const std::vector<AudioFrame> blocks = heard.wait_for_more(300ms);
    Duration heard_for{};
    for (const AudioFrame& block : blocks) heard_for += block.duration();
    EXPECT_GE(heard_for, 300ms);
    for (size_t i = 1; i < blocks.size(); ++i) {
        const Timestamp follows_on = blocks[i - 1].capture_time() + blocks[i - 1].duration();
        EXPECT_LE(std::chrono::abs(blocks[i].capture_time() - follows_on), 2ms) << "block " << i;
    }
}
