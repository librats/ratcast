// pipewire::AudioPlayback and pipewire::AudioCapture on the user's PipeWire
// daemon, each the other's test: a playback of the tone, and a capture of that
// playback's node, which hears the tone in blocks that follow on in time — and
// ends when the node goes, or when there never was one; and a capture of the
// default output, which hears the playback too. Skipped where there is no
// PipeWire (CI).

#include "linux/pipewire_audio.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "ratcast/engine/tone_source.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr auto kPatience = 5s;
constexpr AudioFormat kMono{48'000, 1};
const ToneConfig kTone{kMono, 440.0f, 0.25f};

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
    /// nothing after kPatience. Silence before the tone (the link being made)
    /// is left out.
    std::vector<AudioFrame> wait_for_tone(Duration duration) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, kPatience, [&] { return tone_heard() >= duration; });
        std::vector<AudioFrame> tone;
        for (const AudioFrame& block : blocks_) {
            if (!tone.empty() || loud(block)) tone.push_back(block);
        }
        return tone;
    }

    std::optional<Result<void>> wait_for_end() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, kPatience, [&] { return outcome_.has_value(); });
        return outcome_;
    }

private:
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

float rms(const std::vector<AudioFrame>& blocks) {
    double sum = 0;
    size_t count = 0;
    for (const AudioFrame& block : blocks) {
        for (float s : block.samples()) sum += double(s) * s;
        count += block.samples().size();
    }
    return count ? float(std::sqrt(sum / count)) : 0.0f;
}

class PipeWireAudioTest : public ::testing::Test {
protected:
    void SetUp() override {
        playback_ = std::make_unique<pipewire::AudioPlayback>(AudioOutputOptions{kMono});
        const Result<void> started = playback_->start(
            [this](std::span<float> out, Timestamp play_time) {
                const size_t length = out.size();
                const AudioFrame tone = draw_tone(kTone, played_, length, play_time);
                std::copy(tone.samples().begin(), tone.samples().end(), out.begin());
                played_ += length;
                if (play_time < SteadyClock::instance().now()) early_ = true;
            },
            {});
        if (!started) GTEST_SKIP() << "no PipeWire daemon here";
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while (playback_->node_id() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        ASSERT_NE(playback_->node_id(), 0u);
    }

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
    pipewire::AudioCapture capture({.format = kMono, .system_sound = true});
    Heard heard;
    ASSERT_TRUE(capture.start(heard.sink(), heard.ended()));
    const std::vector<AudioFrame> tone = heard.wait_for_tone(200ms);
    if (tone.empty()) GTEST_SKIP() << "no default output here to listen to";
    EXPECT_GT(rms(tone), 0.05f);  // at whatever volume the output is set to
}
