// aaudio::AudioPlayback and aaudio::AudioCapture on the device's own sound: a
// playback of the tone pulled with play times that lie ahead and run on, one
// stopped and started again; the microphone heard in blocks that follow on,
// stamped no later than now; a call's playback and microphone (voice
// communication's) the same; what other applications play, and nodes,
// Unavailable without a MediaProjection. Skipped where there is no sound
// output, or no microphone (or no RECORD_AUDIO: adb's shell has it).

#include "android/aaudio.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include "heard_sound.h"
#include "ratcast/engine/tone_source.h"

using namespace ratcast;
using namespace ratcast::test;
using namespace std::chrono_literals;

namespace {

constexpr AudioFormat kMono{48'000, 1};
const ToneConfig kTone{kMono, 440.0f, 0.25f};

/// A playback of the tone, keeping each pull's play time and length.
class Tone {
public:
    explicit Tone(AudioOutputOptions options = {.format = kMono}) : playback_(options) {}
    ~Tone() { stop(); }  // before the pulls it writes to go

    struct Pull {
        Timestamp play_time;
        Timestamp pulled_at;
        size_t length;
    };

    bool play() {
        return bool(playback_.start(
            [this](std::span<float> out, Timestamp play_time) {
                const Timestamp now = SteadyClock::instance().now();
                const AudioFrame tone = draw_tone(kTone, played_, out.size(), play_time);
                std::copy(tone.samples().begin(), tone.samples().end(), out.begin());
                played_ += out.size();
                std::lock_guard lock(mutex_);
                pulls_.push_back({play_time, now, out.size()});
            },
            {}));
    }
    void stop() { playback_.stop(); }

    std::vector<Pull> pulls() {
        std::lock_guard lock(mutex_);
        return pulls_;
    }

private:
    aaudio::AudioPlayback playback_;
    uint64_t played_ = 0;  ///< on the playback's thread
    std::mutex mutex_;
    std::vector<Pull> pulls_;
};

}  // namespace

TEST(AAudioTest, APlaybackIsPulledWithPlayTimesAheadThatRunOn) {
    Tone tone;
    if (!tone.play()) GTEST_SKIP() << "no sound output here";
    std::this_thread::sleep_for(500ms);
    tone.stop();

    const std::vector<Tone::Pull> pulls = tone.pulls();
    ASSERT_GT(pulls.size(), 10u);
    size_t played = 0;
    for (const Tone::Pull& pull : pulls) played += pull.length;
    EXPECT_NEAR(double(played), 48'000 * 0.5, 48'000 * 0.15) << "half a second's worth";

    // Once the stream stamps its frames (a few pulls in), each pull is heard
    // later than it is made, where the one before it ends.
    for (size_t i = pulls.size() / 2; i < pulls.size(); ++i) {
        EXPECT_GT(pulls[i].play_time, pulls[i].pulled_at) << "pull " << i;
        const Timestamp follows_on =
            pulls[i - 1].play_time + audio_duration(pulls[i - 1].length, kMono.sample_rate);
        EXPECT_LE(std::chrono::abs(pulls[i].play_time - follows_on), 2ms) << "pull " << i;
    }
    const Tone::Pull& last = pulls.back();
    EXPECT_LT(last.play_time - last.pulled_at, 200ms) << "played far later than asked";
}

TEST(AAudioTest, APlaybackStopsAndStartsAgain) {
    Tone tone;
    if (!tone.play()) GTEST_SKIP() << "no sound output here";
    EXPECT_FALSE(tone.play()) << "already started";
    tone.stop();
    EXPECT_TRUE(tone.play());
}

TEST(AAudioTest, ADeviceThatIsNoIdPlaysToTheDefault) {
    Tone tone({.format = kMono, .device = "not-a-device"});
    if (!tone.play()) GTEST_SKIP() << "no sound output here";
}

TEST(AAudioTest, AnOutputOfANodeIsUnavailable) {
    aaudio::AudioPlayback playback({.format = kMono, .node = 42});
    const Result<void> started = playback.start([](std::span<float>, Timestamp) {}, {});
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error(), Errc::Unavailable);
}

TEST(AAudioTest, TheMicrophoneIsHeardInBlocksThatFollowOn) {
    aaudio::AudioCapture capture({.format = kMono});
    for (int round = 0; round < 2; ++round) {
        Heard heard;
        if (!capture.start(heard.sink(), heard.ended())) GTEST_SKIP() << "no microphone here";
        EXPECT_FALSE(capture.start(heard.sink(), heard.ended())) << "already started";
        const std::vector<AudioFrame> blocks = heard.wait_for_more(300ms);
        capture.stop();

        ASSERT_FALSE(blocks.empty()) << "round " << round;
        for (size_t i = 1; i < blocks.size(); ++i) {
            EXPECT_EQ(blocks[i].format(), kMono);
            const Timestamp follows_on = blocks[i - 1].capture_time() + blocks[i - 1].duration();
            EXPECT_LE(std::chrono::abs(blocks[i].capture_time() - follows_on), 2ms)
                << "block " << i;
        }
        EXPECT_LE(blocks.back().capture_time() + blocks.back().duration(),
                  SteadyClock::instance().now());
    }
}

TEST(AAudioTest, ACallsSoundPlaysAndItsMicrophoneIsHeardAsAnyOther) {
    Tone tone({.format = kMono, .call = true});
    if (!tone.play()) GTEST_SKIP() << "no sound output here";
    aaudio::AudioCapture capture({.format = kMono, .call = true});
    Heard heard;
    if (!capture.start(heard.sink(), heard.ended())) GTEST_SKIP() << "no microphone here";
    const std::vector<AudioFrame> blocks = heard.wait_for_more(300ms);
    capture.stop();
    tone.stop();

    ASSERT_FALSE(blocks.empty());
    for (size_t i = 1; i < blocks.size(); ++i) {
        const Timestamp follows_on = blocks[i - 1].capture_time() + blocks[i - 1].duration();
        EXPECT_LE(std::chrono::abs(blocks[i].capture_time() - follows_on), 2ms) << "block " << i;
    }
    const std::vector<Tone::Pull> pulls = tone.pulls();
    ASSERT_GT(pulls.size(), 10u);
    for (size_t i = pulls.size() / 2; i < pulls.size(); ++i)
        EXPECT_GT(pulls[i].play_time, pulls[i].pulled_at) << "pull " << i;
}

TEST(AAudioTest, WhatOtherApplicationsPlayIsUnavailable) {
    for (const AudioCaptureSource source :
         {AudioCaptureSource::SystemSound, AudioCaptureSource::Applications}) {
        aaudio::AudioCapture capture({.format = kMono, .source = source});
        Heard heard;
        const Result<void> started = capture.start(heard.sink(), heard.ended());
        ASSERT_FALSE(started);
        EXPECT_EQ(started.error(), Errc::Unavailable);
    }
}
