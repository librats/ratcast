// A tone published on one session and played on another, joined by a
// loopback, into an output without a device: what is heard is checked as
// sound — a tone at its level — and as the player's and publisher's stats say;
// and with the test pattern beside it, played in step with it.
// Real threads and real time, so the checks leave room for a slow machine.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "loopback.h"
#include "ratcast/client/audio_player.h"
#include "ratcast/client/audio_publisher.h"
#include "ratcast/client/playout_clock.h"
#include "ratcast/client/video_player.h"
#include "ratcast/client/video_publisher.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/tone_source.h"
#include "ratcast/engine/virtual_output.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

/// The tone's peak: loud enough for Opus to take a steady tone for speech,
/// which is what it codes FEC for (SILK's voice activity; a tone below about
/// 0.45 is not).
constexpr float kLevel = 0.5f;

PeerId make_peer(uint8_t seed) {
    std::array<uint8_t, 32> key{};
    key.fill(seed);
    return PeerId::from_public_key(key.data(), key.size());
}

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

class AudioPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!open_audio_encoder({})) GTEST_SKIP() << "no Opus here";

        sender_ = &loopback_.add(sender_id_, SessionHandlers{
                                                 .keyframe_needed =
                                                     [this](StreamId) {
                                                         if (VideoPublisher* p = pictures_.load())
                                                             p->request_keyframe();
                                                     },
                                             });
        receiver_ = &loopback_.add(
            receiver_id_,
            SessionHandlers{
                .stream_announced =
                    [this](const PeerId&, const StreamDescription& description) {
                        std::lock_guard lock(mutex_);
                        (description.kind() == MediaKind::Audio ? announced_ : video_announced_) =
                            description;
                    },
                .stream_withdrawn = [this](const PeerId&, StreamId) { withdrawn_ = true; },
                .frame_received =
                    [this](const PeerId&, EncodedFrame frame) {
                        if (frame.info.stream == kVideo) {
                            if (VideoPlayer* p = video_ptr_.load()) p->push(std::move(frame));
                            return;
                        }
                        if (lose_ && lose_(frame.info.number)) return;
                        if (AudioPlayer* p = player_ptr_.load()) p->push(std::move(frame));
                    },
            });
        loopback_.connect(sender_id_, receiver_id_);
    }

    void TearDown() override {
        player_ptr_ = nullptr;
        video_ptr_ = nullptr;
        if (player_) player_->stop();
        if (video_player_) video_player_->stop();
        if (publisher_) publisher_->stop();
        pictures_ = nullptr;
        if (video_publisher_) video_publisher_->stop();
        loopback_.stop();  // its handlers use what the test is about to destroy
    }

    void publish() {
        publisher_ = std::make_unique<AudioPublisher>(*sender_, AudioPublisherConfig{});
        ASSERT_TRUE(publisher_->start(tone_));
    }

    /// Once the stream is announced: a player for it, started.
    void play(AudioPlayerConfig config = {}) {
        ASSERT_TRUE(eventually([&] {
            std::lock_guard lock(mutex_);
            return announced_.has_value();
        }));
        StreamDescription description;
        {
            std::lock_guard lock(mutex_);
            description = *announced_;
        }
        player_ =
            std::make_unique<AudioPlayer>(*receiver_, sender_id_, description, output_, config,
                                          [this](Timestamp capture_time, Timestamp play_time) {
                                              std::lock_guard lock(mutex_);
                                              latencies_.push_back(play_time - capture_time);
                                          });
        ASSERT_TRUE(player_->start());
        player_ptr_ = player_.get();
    }

    /// The pattern beside the sound, and a player for it, in step with the
    /// sound's.
    void publish_and_play_pictures() {
        VideoPublisherConfig config;
        config.stream = kVideo;
        config.encoder.bitrate = 500'000;
        video_publisher_ = std::make_unique<VideoPublisher>(*sender_, config);
        pictures_ = video_publisher_.get();
        ASSERT_TRUE(video_publisher_->start(pattern_));

        ASSERT_TRUE(eventually([&] {
            std::lock_guard lock(mutex_);
            return video_announced_.has_value();
        }));
        StreamDescription description;
        {
            std::lock_guard lock(mutex_);
            description = *video_announced_;
        }
        video_player_ = std::make_unique<VideoPlayer>(
            *receiver_, sender_id_, description, [](VideoFrame) {},
            VideoPlayerConfig{.sync = &sync_});
        ASSERT_TRUE(video_player_->start());
        video_ptr_ = video_player_.get();
    }

    /// The level of what was heard, 20 ms at a time, from the first frame on.
    std::vector<float> levels() {
        std::lock_guard lock(mutex_);
        std::vector<float> out;
        const auto first = std::ranges::find_if(heard_, [](float s) { return s != 0.0f; });
        const size_t window = 960 * 2;  // 20 ms of 48 kHz stereo
        for (auto at = first; heard_.end() - at >= ptrdiff_t(window); at += window) {
            double sum = 0;
            for (auto s = at; s != at + window; ++s) sum += double(*s) * *s;
            out.push_back(float(std::sqrt(sum / window)));
        }
        return out;
    }

    test::Loopback loopback_;
    const PeerId sender_id_ = make_peer(1);
    const PeerId receiver_id_ = make_peer(2);
    MediaSession* sender_ = nullptr;
    MediaSession* receiver_ = nullptr;

    ToneSource tone_{{.format = {48'000, 1}, .level = kLevel}};
    VirtualOutput output_{{.format = {48'000, 2}},
                          [this](std::span<const float> samples, Timestamp) {
                              std::lock_guard lock(mutex_);
                              heard_.insert(heard_.end(), samples.begin(), samples.end());
                          }};
    std::unique_ptr<AudioPublisher> publisher_;
    std::unique_ptr<AudioPlayer> player_;
    std::atomic<AudioPlayer*> player_ptr_{nullptr};
    std::function<bool(FrameNumber)> lose_;  ///< frames the receiver never sees

    static constexpr StreamId kVideo = 1;
    PatternSource pattern_{{PixelFormat::NV12, 320, 240, 30, 1}};
    std::unique_ptr<VideoPublisher> video_publisher_;
    std::atomic<VideoPublisher*> pictures_{nullptr};
    std::unique_ptr<VideoPlayer> video_player_;
    std::atomic<VideoPlayer*> video_ptr_{nullptr};
    PlayoutClock sync_;

    std::mutex mutex_;
    std::optional<StreamDescription> announced_;
    std::optional<StreamDescription> video_announced_;
    std::atomic<bool> withdrawn_{false};
    std::vector<float> heard_;
    std::vector<Duration> latencies_;
};

}  // namespace

TEST_F(AudioPipelineTest, PlaysTheToneAsItWasSent) {
    publish();
    play();
    ASSERT_TRUE(eventually([&] { return levels().size() >= 50; }));  // a second of it
    player_->stop();

    {
        std::lock_guard lock(mutex_);
        EXPECT_EQ(announced_->codec, Codec::Opus);
        EXPECT_EQ(*announced_->audio(), (AudioFormat{48'000, 1}));
        EXPECT_EQ(announced_->label, "sound");
    }
    const AudioPublisherStats sent = publisher_->stats();
    EXPECT_GE(sent.frames, 50u);
    EXPECT_EQ(sent.dropped, 0u);
    EXPECT_FALSE(sent.encoder.empty());

    const AudioPlayerStats played = player_->stats();
    EXPECT_GE(played.jitter.played, 45u);
    EXPECT_EQ(played.jitter.lost, 0u);
    EXPECT_EQ(played.decode_errors, 0u);
    EXPECT_FALSE(played.decoder.empty());

    // The tone, mono to both channels, at its level (RMS of a sine: peak/√2);
    // a slow machine may have had a frame or two made up on the way.
    std::vector<float> rms = levels();
    const float expected = kLevel / std::numbers::sqrt2_v<float>;
    std::ranges::nth_element(rms, rms.begin() + rms.size() / 2);
    EXPECT_NEAR(rms[rms.size() / 2], expected, expected * 0.15f);

    // From capture to the speaker: the jitter buffer's delay, the output's
    // block and its latency, and no more than a slow machine adds.
    std::lock_guard lock(mutex_);
    ASSERT_FALSE(latencies_.empty());
    std::ranges::nth_element(latencies_, latencies_.begin() + latencies_.size() / 2);
    EXPECT_GE(latencies_[latencies_.size() / 2], 20ms);
    EXPECT_LE(latencies_[latencies_.size() / 2], 150ms);
}

TEST_F(AudioPipelineTest, FramesLostOnTheWayAreMadeUpWithoutABreak) {
    // One frame in 25 never arrives, once playing has started.
    lose_ = [](FrameNumber n) { return n > 25 && n % 25 == 10; };
    publish();
    play();
    ASSERT_TRUE(eventually([&] { return levels().size() >= 100; }));
    player_->stop();

    const AudioPlayerStats played = player_->stats();
    EXPECT_GE(played.jitter.lost, 2u);
    EXPECT_GE(played.decoded.recovered + played.decoded.concealed,
              20ms * int64_t(played.jitter.lost));
    // The next frame's FEC brings each lost one back: the tone never dips, as
    // it would where a frame is concealed (decoder stats cannot tell the two
    // apart before libopus 1.5; the sound can). A slow machine may miss one.
    const float expected = kLevel / std::numbers::sqrt2_v<float>;
    const std::vector<float> rms = levels();
    const auto quiet = std::ranges::count_if(rms, [&](float r) { return r < expected / 2; });
    // Where, and what the player made of it: a loss concealed rather than
    // recovered, or sound made up while the next frame was not there.
    std::string where;
    for (size_t i = 0; i < rms.size(); ++i) {
        if (rms[i] < expected / 2) where += " " + std::to_string(i);
    }
    const auto ms = [](Duration d) { return std::to_string(d.count() / 1000) + " ms"; };
    EXPECT_LE(quiet, 1) << "of " << rms.size() << " windows of 20 ms, quiet at" << where
                        << "; lost " << played.jitter.lost << ", late " << played.jitter.late
                        << ", dropped " << played.jitter.dropped << ", made up "
                        << ms(played.jitter.concealed) << " + " << ms(played.jitter.expanded)
                        << " expanded; recovered " << ms(played.decoded.recovered) << ", concealed "
                        << ms(played.decoded.concealed) << "; delay " << ms(played.delay) << " for "
                        << ms(played.target);
}

TEST_F(AudioPipelineTest, StoppingWithdrawsTheStreamAndThePlayerFallsSilent) {
    publish();
    play();
    ASSERT_TRUE(eventually([&] {
        std::lock_guard lock(mutex_);
        return latencies_.size() >= 5;
    }));
    player_->stop();
    size_t frames = 0;
    {
        std::lock_guard lock(mutex_);
        frames = latencies_.size();
    }
    std::this_thread::sleep_for(50ms);
    {
        std::lock_guard lock(mutex_);
        EXPECT_EQ(latencies_.size(), frames);
    }

    publisher_->stop();
    EXPECT_TRUE(eventually([&] { return withdrawn_.load(); }));
    // And again, from the start.
    withdrawn_ = false;
    ASSERT_TRUE(publisher_->start(tone_));
    EXPECT_FALSE(publisher_->start(tone_));
}

TEST_F(AudioPipelineTest, APlayerTakesOnlyAnAudioStream) {
    StreamDescription video;
    video.id = 1;
    video.codec = Codec::AV1;
    video.format = VideoFormat{320, 240};
    video.lifetime = 200ms;
    AudioPlayer player(*receiver_, sender_id_, video, output_);
    EXPECT_FALSE(player.start());
}

TEST_F(AudioPipelineTest, PicturesPlayInStepWithTheSound) {
    VideoEncoderConfig probe;
    probe.width = 320;
    probe.height = 240;
    if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

    publish();
    play({.sync = &sync_});
    publish_and_play_pictures();
    // Two seconds of pictures shown with sound playing.
    ASSERT_TRUE(eventually([&] { return sync_.stats().apart.count() >= 60; }, 15s));
    video_ptr_ = nullptr;
    video_player_->stop();

    // On a loopback the pictures would come some 50 ms before the sound —
    // its frame, its jitter buffer, the output — and are held for it.
    const PlayoutClockStats stats = sync_.stats();
    ASSERT_TRUE(stats.sound);
    EXPECT_GE(*stats.sound, 30ms);
    EXPECT_LE(stats.apart.percentile(0.5), 15ms);
    EXPECT_LE(stats.apart.percentile(0.95), 40ms);
}
