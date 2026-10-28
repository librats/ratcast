// A publisher and a player on two sessions joined by a loopback: pictures go
// in on one side and come out decoded on the other. The pattern's stamps say
// which picture came out; the publisher's and player's stats say how.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "loopback.h"
#include "ratcast/client/video_player.h"
#include "ratcast/client/video_publisher.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/real_time.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

PeerId make_peer(uint8_t seed) {
    std::array<uint8_t, 32> key{};
    key.fill(seed);
    return PeerId::from_public_key(key.data(), key.size());
}

/// A source the test drives by hand, one picture at a time.
class ManualSource final : public VideoSource {
public:
    Result<void> start(Sink sink, EndHandler) override {
        std::lock_guard lock(mutex_);
        sink_ = std::move(sink);
        return {};
    }
    void stop() override {
        std::lock_guard lock(mutex_);
        sink_ = nullptr;
    }
    void show(VideoFrame picture) {
        std::lock_guard lock(mutex_);
        if (sink_) sink_(std::move(picture));
    }

private:
    std::mutex mutex_;
    Sink sink_;
};

/// The stamp on picture `i` of a test.
Timestamp stamp(uint64_t i) { return Timestamp{10s + std::chrono::milliseconds(i)}; }

VideoFrame picture(uint64_t i, uint32_t width = 320, uint32_t height = 240) {
    return draw_pattern({PixelFormat::NV12, width, height}, i, stamp(i));
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

class VideoPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        VideoEncoderConfig probe;
        probe.width = 320;
        probe.height = 240;
        if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

        sender_ = &loopback_.add(sender_id_, SessionHandlers{
                                                 .keyframe_needed =
                                                     [this](StreamId) {
                                                         if (VideoPublisher* p = publisher_.get())
                                                             p->request_keyframe();
                                                     },
                                             });
        receiver_ = &loopback_.add(
            receiver_id_,
            SessionHandlers{
                .stream_announced =
                    [this](const PeerId&, const StreamDescription& description) {
                        {
                            std::lock_guard lock(mutex_);
                            announced_.push_back(description);
                        }
                        if (VideoPlayer* p = player_ptr_.load()) p->update(description);
                    },
                .stream_withdrawn = [this](const PeerId&, StreamId) { withdrawn_ = true; },
                .frame_received =
                    [this](const PeerId&, EncodedFrame frame) {
                        if (VideoPlayer* p = player_ptr_.load()) p->push(std::move(frame));
                    },
            });
        loopback_.connect(sender_id_, receiver_id_);

        VideoPublisherConfig config;
        config.encoder.bitrate = 1'000'000;
        publisher_ = std::make_unique<VideoPublisher>(*sender_, config);
    }

    void TearDown() override {
        player_ptr_ = nullptr;
        if (player_) player_->stop();
        if (publisher_) publisher_->stop();
        loopback_.stop();  // its handlers use what the test is about to destroy
    }

    /// Once the stream is announced: a player for it, started.
    void play() {
        ASSERT_TRUE(eventually([&] {
            std::lock_guard lock(mutex_);
            return !announced_.empty();
        }));
        StreamDescription description;
        {
            std::lock_guard lock(mutex_);
            description = announced_.back();
        }
        player_ = std::make_unique<VideoPlayer>(*receiver_, sender_id_, description,
                                                [this](VideoFrame picture) {
                                                    std::lock_guard lock(mutex_);
                                                    pictures_.push_back(std::move(picture));
                                                });
        player_ptr_ = player_.get();
        ASSERT_TRUE(player_->start());
    }

    size_t picture_count() {
        std::lock_guard lock(mutex_);
        return pictures_.size();
    }
    std::vector<VideoFrame> pictures() {
        std::lock_guard lock(mutex_);
        return pictures_;
    }
    /// Until a picture `match` accepts has come out.
    bool wait_for_picture(const std::function<bool(const VideoFrame&)>& match) {
        return eventually([&] {
            std::lock_guard lock(mutex_);
            for (const VideoFrame& p : pictures_) {
                if (match(p)) return true;
            }
            return false;
        });
    }

    const PeerId sender_id_ = make_peer(1);
    const PeerId receiver_id_ = make_peer(2);
    test::Loopback loopback_;
    MediaSession* sender_ = nullptr;
    MediaSession* receiver_ = nullptr;

    std::mutex mutex_;
    std::vector<StreamDescription> announced_;
    std::vector<VideoFrame> pictures_;
    std::atomic<bool> withdrawn_{false};

    // After the loopback, so that they go first; the sources before what uses them.
    ManualSource source_;
    PatternSource pattern_{{PixelFormat::NV12, 320, 240, 60, 1}};
    std::unique_ptr<VideoPublisher> publisher_;
    std::unique_ptr<VideoPlayer> player_;
    std::atomic<VideoPlayer*> player_ptr_{nullptr};
};

}  // namespace

TEST_F(VideoPipelineTest, PicturesTravelFromSourceToSink) {
    ASSERT_TRUE(publisher_->start(pattern_));
    play();
    ASSERT_TRUE(eventually([&] { return picture_count() >= 20; }));
    publisher_->stop();

    std::optional<Timestamp> previous;
    for (const VideoFrame& p : pictures()) {
        EXPECT_EQ(p.width(), 320u);
        EXPECT_EQ(p.height(), 240u);
        const std::optional<Timestamp> t = read_timestamp(p);
        ASSERT_TRUE(t);
        if (previous) {
            EXPECT_GT(*t, *previous);  // in order, none twice: a live source is never repeated
        }
        previous = t;
    }
    const VideoPublisherStats sent = publisher_->stats();
    EXPECT_GE(sent.frames, 20u);
    EXPECT_EQ(sent.encoders, 1u);
    EXPECT_FALSE(sent.encoder.empty());
    const VideoPlayerStats played = player_->stats();
    EXPECT_GE(played.pictures, 20u);
    EXPECT_EQ(played.decode_errors, 0u);
    EXPECT_FALSE(played.decoder.empty());
}

TEST_F(VideoPipelineTest, AStillScreenStillReachesANewViewer) {
    ASSERT_TRUE(publisher_->start(source_));
    for (uint64_t i = 0; i < 3; ++i) source_.show(picture(i));
    ASSERT_TRUE(eventually([&] {
        const VideoPublisherStats s = publisher_->stats();
        return s.frames + s.skipped == 3;  // the last picture, 2, among those encoded
    }));

    // Nothing new on screen; the viewer still sees what is there.
    play();
    ASSERT_TRUE(
        wait_for_picture([](const VideoFrame& p) { return read_timestamp(p) == stamp(2); }));
    EXPECT_GE(publisher_->stats().repeated, 1u);
}

TEST_F(VideoPipelineTest, FollowsTheSourceToANewSize) {
    ASSERT_TRUE(publisher_->start(source_));
    source_.show(picture(0));
    play();
    uint64_t i = 1;
    ASSERT_TRUE(eventually([&] {
        source_.show(picture(i++));
        return picture_count() > 0;
    }));

    ASSERT_TRUE(eventually([&] {
        source_.show(picture(i++, 640, 360));
        std::this_thread::sleep_for(10ms);
        return wait_for_picture([](const VideoFrame& p) { return p.width() == 640; });
    }));
    EXPECT_EQ(publisher_->stats().encoders, 2u);
    std::lock_guard lock(mutex_);
    ASSERT_GE(announced_.size(), 2u);
    EXPECT_EQ(announced_.back().video()->width, 640u);
    EXPECT_EQ(announced_.back().video()->height, 360u);
}

TEST_F(VideoPipelineTest, KeepsOnlyTheNewestPictureWhenTheEncoderIsBusy) {
    std::vector<VideoFrame> burst;
    for (uint64_t i = 0; i < 40; ++i) burst.push_back(picture(i));
    ASSERT_TRUE(publisher_->start(source_));
    for (VideoFrame& p : burst) source_.show(std::move(p));

    ASSERT_TRUE(eventually([&] {
        const VideoPublisherStats s = publisher_->stats();
        return s.frames + s.skipped == 40;
    }));
    const VideoPublisherStats s = publisher_->stats();
    EXPECT_EQ(s.pictures, 40u);
    EXPECT_GT(s.skipped, 0u);
}

TEST_F(VideoPipelineTest, StoppingWithdrawsTheStream) {
    ASSERT_TRUE(publisher_->start(source_));
    source_.show(picture(0));
    play();
    publisher_->stop();
    EXPECT_TRUE(eventually([&] { return withdrawn_.load(); }));
}

TEST_F(VideoPipelineTest, ADamagedFrameAsksForAKeyframe) {
    ASSERT_TRUE(publisher_->start(pattern_));
    play();
    ASSERT_TRUE(eventually([&] { return picture_count() >= 3; }));

    EncodedFrame garbage;
    garbage.info.keyframe = true;
    garbage.payload = Buffer::copy_of(std::vector<uint8_t>(64, 0xFF));
    player_->push(garbage);
    ASSERT_TRUE(eventually([&] { return player_->stats().decode_errors >= 1; }));
    EXPECT_GE(player_->stats().keyframe_requests, 1u);

    const size_t before = picture_count();
    EXPECT_TRUE(eventually([&] { return picture_count() > before + 5; }));  // and it goes on
}

TEST_F(VideoPipelineTest, PlaysEvenlyPastFramesThatCameLate) {
    // A stream to subscribe to; its own frames go nowhere, the player is fed
    // by hand below.
    ASSERT_TRUE(publisher_->start(pattern_));
    ASSERT_TRUE(eventually([&] {
        std::lock_guard lock(mutex_);
        return !announced_.empty();
    }));
    StreamDescription description;
    {
        std::lock_guard lock(mutex_);
        description = announced_.back();
    }
    std::vector<Timestamp> shown;
    VideoPlayer player(*receiver_, sender_id_, description, [&](VideoFrame) {
        std::lock_guard lock(mutex_);
        shown.push_back(SteadyClock::instance().now());
    });
    ASSERT_TRUE(player.start());

    // 60 frames at 60 fps, as a lossy path delivers them: frames 15, 30 and
    // 45 are sent again and come 80, 70 and 60 ms late, and the frames behind
    // each come with it.
    VideoEncoderConfig config;
    config.width = 320;
    config.height = 240;
    Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config);
    ASSERT_TRUE(encoder);
    // Encoded beforehand, so that only the path's lateness is in their timing.
    constexpr Duration kFrame = 16'667us;
    std::vector<std::vector<EncodedFrame>> encoded(60);
    for (uint64_t i = 0; i < encoded.size(); ++i) {
        const VideoFrame frame = draw_pattern({(*encoder)->info().input_format, 320, 240}, i,
                                              Timestamp{10s} + int64_t(i) * kFrame);
        ASSERT_TRUE((*encoder)->encode(frame, i == 0, [&](EncodedVideo&& v) {
            encoded[i].push_back({v.info, Buffer::copy_of(v.payload.bytes())});
        }));
    }
    // Fed in time, as the path would: these waits must end when due too.
    keep_time_on_this_thread();
    const Timestamp start = SteadyClock::instance().now();
    Timestamp held_until{};
    for (uint64_t i = 0; i < encoded.size(); ++i) {
        const Timestamp due = start + int64_t(i) * kFrame;
        if (i == 15) held_until = due + 80ms;
        if (i == 30) held_until = due + 70ms;
        if (i == 45) held_until = due + 60ms;
        std::this_thread::sleep_until(std::max(due, held_until));
        for (EncodedFrame& f : encoded[i]) player.push(std::move(f));
    }
    ASSERT_TRUE(eventually([&] { return player.stats().pictures == 60; }));
    player.stop();

    // The first late run is a stall, and so is the second, which says the
    // path does it: the delay then covers the third.
    std::vector<Timestamp> times;
    {
        std::lock_guard lock(mutex_);
        times = shown;
    }
    const auto longest_gap = [&](size_t from, size_t to) {
        Duration longest{};
        for (size_t i = from + 1; i < to; ++i) longest = std::max(longest, times[i] - times[i - 1]);
        return longest;
    };
    EXPECT_GE(longest_gap(10, 25), 70ms);
    EXPECT_LT(longest_gap(40, 60), 45ms);  // shown as they came, one would be 76 ms
    EXPECT_GE(player.stats().delay, 65ms);
}

TEST_F(VideoPipelineTest, AimsWhereRateControlAsks) {
    ASSERT_TRUE(publisher_->start(pattern_));
    play();
    ASSERT_TRUE(eventually([&] { return picture_count() >= 3; }));
    // Rate control starts at its start, capped at the encoder's bitrate.
    EXPECT_EQ(publisher_->stats().bitrate, 1'000'000u);
    EXPECT_EQ(sender_->outgoing_stats(1)->bitrate, 1'000'000u);

    publisher_->set_bitrate(400'000);
    ASSERT_TRUE(eventually([&] { return publisher_->stats().bitrate == 400'000u; }));
    const size_t before = picture_count();
    EXPECT_TRUE(eventually([&] { return picture_count() > before + 10; }));
    EXPECT_EQ(player_->stats().decode_errors, 0u);
}
