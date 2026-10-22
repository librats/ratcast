// Calls on loopback, between real librats nodes: each side sending and
// playing, a camera beside the screen, hanging up, streams withdrawn and
// offered again, sources that run out, and the invites a call hands out.

#include "ratcast/client/call.h"
#include "ratcast/client/camera.h"
#include "ratcast/client/screen_share.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/tone_source.h"
#include "ratcast/engine/virtual_output.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

/// A call on loopback, of this test's own protocol: calls of other tests
/// running alongside never connect to it.
CallConfig config(std::optional<RatsUrl> dial = std::nullopt) {
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    return CallConfig{
        .bind_address = "127.0.0.1",
        .dial = std::move(dial),
        .protocol = std::string("ratcast-test/") + test->test_suite_name() + "." + test->name(),
        .watch_network = false,
    };
}

/// Sound that runs out: a few blocks of the tone, then the end.
class ShortSound final : public AudioSource {
public:
    ~ShortSound() override { stop(); }

    const AudioFormat& format() const noexcept override { return tone_.format; }

    Result<void> start(Sink sink, EndHandler ended) override {
        thread_ = std::thread([this, sink = std::move(sink), ended = std::move(ended)] {
            for (uint64_t block = 0; block < 5; ++block) {
                sink(draw_tone(tone_, block * 480, 480, SteadyClock::instance().now()));
            }
            ended({});
        });
        return {};
    }

    void stop() override {
        if (thread_.joinable()) thread_.join();
    }

private:
    const ToneConfig tone_{};
    std::thread thread_;
};

/// One side of a call: what it played and what it was told.
class Side {
public:
    ~Side() { hang_up(); }

    void start(CallConfig config, bool pictures = true, bool camera = true) {
        if (pictures) {
            config.playback.pictures = [this](VideoFrame) {
                std::lock_guard lock(mutex_);
                ++pictures_;
            };
        }
        if (camera) {
            config.playback.camera = [this](VideoFrame) {
                std::lock_guard lock(mutex_);
                ++camera_pictures_;
            };
        }
        config.playback.sound = &speaker_;
        config.playback.heard = [this](Timestamp, Timestamp) {
            std::lock_guard lock(mutex_);
            ++heard_;
        };
        Result<std::unique_ptr<Call>> started = Call::start(std::move(config), handlers());
        ASSERT_TRUE(started) << to_string(started.error());
        call_ = std::move(*started);
    }

    void publish_pattern() {
        VideoPublisherConfig video;
        video.label = "pattern";
        video.encoder.bitrate = 1'000'000;
        video.start_bitrate = 1'000'000;
        ASSERT_TRUE(call_->publish(
            std::make_unique<PatternSource>(PatternConfig{PixelFormat::NV12, 320, 240, 30, 1}),
            video));
    }

    void publish_tone() {
        ASSERT_TRUE(call_->publish(std::make_unique<ToneSource>(ToneConfig{.level = 0.5f}),
                                   AudioPublisherConfig{.label = "tone"}));
    }

    void hang_up() { call_.reset(); }

    Call& operator*() { return *call_; }
    Call* operator->() { return call_.get(); }
    librats::HostEndpoint address() const { return {"127.0.0.1", call_->port()}; }

    size_t pictures() {
        std::lock_guard lock(mutex_);
        return pictures_;
    }
    size_t heard() {
        std::lock_guard lock(mutex_);
        return heard_;
    }
    size_t camera_pictures() {
        std::lock_guard lock(mutex_);
        return camera_pictures_;
    }
    /// What it was told, in order: "joined", "playing tone", "ended tone",
    /// "source 2 ended", ...
    std::vector<std::string> events() {
        std::lock_guard lock(mutex_);
        return events_;
    }
    bool told(const std::string& event) {
        const std::vector<std::string> all = events();
        return std::ranges::find(all, event) != all.end();
    }
    size_t told_times(const std::string& event) {
        return static_cast<size_t>(std::ranges::count(events(), event));
    }

private:
    CallHandlers handlers() {
        return CallHandlers{
            .joined = [this](const PeerId&) { tell("joined"); },
            .left = [this](const PeerId&) { tell("left"); },
            .playing =
                [this](const PeerId&, const StreamDescription& stream, Result<void> outcome) {
                    tell((outcome ? "playing " : "failed ") + stream.label);
                },
            .ended = [this](const PeerId&,
                            const StreamDescription& stream) { tell("ended " + stream.label); },
            .source_ended =
                [this](StreamId stream, Result<void> outcome) {
                    tell("source " + std::to_string(stream) + (outcome ? " ended" : " failed"));
                },
        };
    }

    void tell(std::string event) {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(event));
    }

    std::mutex mutex_;
    size_t pictures_ = 0;
    size_t camera_pictures_ = 0;
    size_t heard_ = 0;
    std::vector<std::string> events_;
    VirtualOutput speaker_{VirtualOutputConfig{}};
    std::unique_ptr<Call> call_;  ///< last: it plays into the rest
};

class CallTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!open_audio_encoder({})) GTEST_SKIP() << "no Opus here";
    }
};

TEST_F(CallTest, BothSidesSendAndPlayUntilOneHangsUp) {
    VideoEncoderConfig probe;
    probe.width = 320;
    probe.height = 240;
    if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

    Side host;
    host.start(config());
    host.publish_pattern();
    host.publish_tone();
    Side guest;
    guest.start(config(RatsUrl{host.address()}));
    guest.publish_pattern();
    guest.publish_tone();

    // Each plays the other's pictures and sound.
    ASSERT_TRUE(wait_for([&] {
        return host.pictures() >= 30 && guest.pictures() >= 30 && host.heard() >= 50 &&
               guest.heard() >= 50;
    })) << "host "
        << host.pictures() << " pictures, " << host.heard() << " heard; guest " << guest.pictures()
        << " pictures, " << guest.heard() << " heard";
    for (Side* side : {&host, &guest}) {
        EXPECT_TRUE(side->told("joined"));
        EXPECT_TRUE(side->told("playing pattern"));
        EXPECT_TRUE(side->told("playing tone"));
        EXPECT_EQ((*side)->peers().size(), 1u);
        const std::optional<ReceivedVideoStats> video = (*side)->video_stats();
        ASSERT_TRUE(video);
        EXPECT_EQ(video->player.decode_errors, 0u);
        EXPECT_EQ(video->player.width, 320u);
        ASSERT_TRUE((*side)->sound_stats());
        EXPECT_GT((*side)->sync_stats().apart.count(), 0u) << "pictures measured against sound";
    }
    EXPECT_EQ(host->peers().front(), guest->id());
    EXPECT_TRUE(wait_for([&] {
        const auto path = guest->path(host->id());
        return path && path->valid;
    }));

    // Hanging up ends what the other side played.
    guest.hang_up();
    EXPECT_TRUE(wait_for([&] {
        return host.told("left") && host.told("ended pattern") && host.told("ended tone");
    })) << ::testing::PrintToString(host.events());
    EXPECT_FALSE(host->video_stats());
    EXPECT_FALSE(host->sound_stats());
}

TEST_F(CallTest, AScreenShareGoesAsPicturesAndMusic) {
    VideoEncoderConfig probe;
    probe.width = 320;
    probe.height = 240;
    if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

    Side host;
    host.start(config());
    ScreenShareConfig share{.window = true};
    share.video.bitrate = 1'000'000;
    ASSERT_TRUE(publish_screen_share(
        *host,
        {.pictures =
             std::make_unique<PatternSource>(PatternConfig{PixelFormat::NV12, 320, 240, 30, 1}),
         .sound = std::make_unique<ToneSource>(ToneConfig{.format = {48'000, 2}})},
        share));
    Side guest;
    guest.start(config(RatsUrl{host.address()}));
    ASSERT_TRUE(wait_for([&] { return guest.pictures() >= 10 && guest.heard() >= 10; }));
    EXPECT_TRUE(guest.told("playing window"));
    EXPECT_TRUE(guest.told("playing sound"));
    const std::optional<OutgoingStreamStats> pictures = host->session().outgoing_stats(1);
    ASSERT_TRUE(pictures);
    EXPECT_GT(pictures->bitrate, 0u) << "rate-controlled";
    EXPECT_LE(pictures->bitrate, 1'000'000u);
    ASSERT_TRUE(guest->sound_stats());
    EXPECT_EQ(guest->sound_stats()->jitter.lost, 0u);
}

TEST_F(CallTest, ACameraPlaysApartFromTheScreen) {
    VideoEncoderConfig probe;
    probe.width = 320;
    probe.height = 240;
    if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

    Side host;
    host.start(config());
    host.publish_pattern();
    std::atomic<size_t> previewed{0};
    ASSERT_TRUE(publish_camera(*host, with_preview(std::make_unique<PatternSource>(PatternConfig{
                                                       PixelFormat::NV12, 160, 120, 30, 1}),
                                                   [&](const VideoFrame&) { ++previewed; })));
    Side guest;
    guest.start(config(RatsUrl{host.address()}));
    ASSERT_TRUE(wait_for([&] { return guest.pictures() >= 10 && guest.camera_pictures() >= 10; }))
        << guest.pictures() << " pictures, " << guest.camera_pictures() << " of the camera";
    EXPECT_TRUE(guest.told("playing pattern"));
    EXPECT_TRUE(guest.told("playing camera"));
    ASSERT_TRUE(guest->video_stats());
    ASSERT_TRUE(guest->camera_stats());
    EXPECT_EQ(guest->video_stats()->player.width, 320u);
    EXPECT_EQ(guest->camera_stats()->player.width, 160u);
    EXPECT_GT(previewed.load(), 0u) << "the sender sees its camera";
    const std::optional<OutgoingStreamStats> camera = host->session().outgoing_stats(3);
    ASSERT_TRUE(camera);
    EXPECT_LE(camera->bitrate, 1'000'000u);

    // The camera off, the screen goes on.
    host->unpublish(3);
    ASSERT_TRUE(wait_for([&] { return guest.told("ended camera"); }));
    EXPECT_FALSE(guest->camera_stats());
    const size_t before = guest.pictures();
    EXPECT_TRUE(wait_for([&] { return guest.pictures() >= before + 10; }));
}

TEST_F(CallTest, ACameraNobodyShowsIsNotReceived) {
    VideoEncoderConfig probe;
    probe.width = 160;
    probe.height = 120;
    if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

    Side host;
    host.start(config());
    ASSERT_TRUE(publish_camera(
        *host, std::make_unique<PatternSource>(PatternConfig{PixelFormat::NV12, 160, 120, 30, 1})));
    host.publish_tone();
    Side guest;
    guest.start(config(RatsUrl{host.address()}), /*pictures=*/true, /*camera=*/false);
    ASSERT_TRUE(wait_for([&] { return guest.told("playing tone"); }));
    // Announced once its first picture is coded.
    ASSERT_TRUE(wait_for([&] { return host->session().outgoing_stats(3).has_value(); }));
    std::this_thread::sleep_for(200ms);  // as long as an answer would take on loopback
    EXPECT_FALSE(guest.told("playing camera")) << "not as the pictures either";
    EXPECT_EQ(host->session().outgoing_stats(3)->subscribers, 0u);
}

TEST_F(CallTest, AScreenShareThatCannotPublishLeavesNothing) {
    Side host;
    host.start(config());
    host.publish_tone();  // stream 2, which the share's sound wants
    const Result<void> shared =
        publish_screen_share(*host,
                             {.pictures = std::make_unique<PatternSource>(PatternConfig{}),
                              .sound = std::make_unique<ToneSource>(ToneConfig{})},
                             {});
    ASSERT_FALSE(shared);
    EXPECT_EQ(shared.error(), Errc::InvalidArgument);
    EXPECT_FALSE(host->session().outgoing_stats(1)) << "the pictures withdrawn";
}

TEST_F(CallTest, AStreamOfferedAgainPlaysAgain) {
    Side host;
    host.start(config());
    host.publish_tone();
    Side guest;
    guest.start(config(RatsUrl{host.address()}), /*pictures=*/false);
    ASSERT_TRUE(wait_for([&] { return guest.heard() >= 10; }));

    host->unpublish(AudioPublisherConfig{}.stream);
    ASSERT_TRUE(wait_for([&] { return guest.told("ended tone"); }));
    EXPECT_FALSE(guest->sound_stats());

    host.publish_tone();
    ASSERT_TRUE(wait_for([&] { return guest.told_times("playing tone") == 2; }))
        << ::testing::PrintToString(guest.events());
    const size_t before = guest.heard();
    EXPECT_TRUE(wait_for([&] { return guest.heard() >= before + 10; }));
}

TEST_F(CallTest, WhatIsNotPlayedIsNotReceived) {
    Side host;
    host.start(config());
    host.publish_tone();
    // No playback at all, as a call that only sends has.
    Result<std::unique_ptr<Call>> silent = Call::start(config(RatsUrl{host.address()}));
    ASSERT_TRUE(silent);
    Side guest;
    guest.start(config(RatsUrl{host.address()}), /*pictures=*/false);
    ASSERT_TRUE(wait_for([&] { return guest.told("playing tone"); }));
    // Two peers, one subscriber: the call that plays nothing was told of the
    // tone but never asked. The guest's request reaches the host in its own
    // time, after the guest has started playing.
    const StreamId tone = AudioPublisherConfig{}.stream;
    EXPECT_TRUE(wait_for([&] { return host->peers().size() == 2; }));
    std::optional<IncomingStreamStats> told;
    EXPECT_TRUE(wait_for([&] {
        told = (*silent)->session().incoming_stats(host->id(), tone);
        return told.has_value();
    }));
    EXPECT_FALSE(told && told->subscribed);
    EXPECT_TRUE(wait_for([&] { return host->session().outgoing_stats(tone)->subscribers == 1; }));
}

TEST_F(CallTest, ASourceThatRunsOutSaysSo) {
    Side host;
    host.start(config());
    ASSERT_TRUE(host->publish(std::make_unique<ShortSound>(), AudioPublisherConfig{.stream = 7}));
    EXPECT_TRUE(wait_for([&] { return host.told("source 7 ended"); }));
    // Published until unpublished: the id is still taken.
    const auto again =
        host->publish(std::make_unique<ShortSound>(), AudioPublisherConfig{.stream = 7});
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error(), Errc::InvalidArgument);
    host->unpublish(7);
    EXPECT_TRUE(host->publish(std::make_unique<ShortSound>(), AudioPublisherConfig{.stream = 7}));
}

TEST_F(CallTest, PublishRefusesNoSourceAndAStreamInUse) {
    Side host;
    host.start(config());
    const auto none = host->publish(std::unique_ptr<AudioSource>{}, AudioPublisherConfig{});
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error(), Errc::InvalidArgument);
    host.publish_tone();
    const auto taken =
        host->publish(std::make_unique<ToneSource>(ToneConfig{}), AudioPublisherConfig{});
    ASSERT_FALSE(taken);
    EXPECT_EQ(taken.error(), Errc::InvalidArgument);
    host->unpublish(99);  // nothing of that id: nothing happens
}

TEST(CallInvites, NameTheAddressAndPortItListensOn) {
    Result<std::unique_ptr<Call>> listening = Call::start(config());
    ASSERT_TRUE(listening);
    const uint16_t port = (*listening)->port();
    EXPECT_NE(port, 0u);
    EXPECT_EQ((*listening)->invites(),
              std::vector<std::string>{"rats://127.0.0.1:" + std::to_string(port)});

    CallConfig dialing = config();
    dialing.listen = false;
    Result<std::unique_ptr<Call>> dialer = Call::start(dialing);
    ASSERT_TRUE(dialer);
    EXPECT_EQ((*dialer)->port(), 0u);
    EXPECT_TRUE((*dialer)->invites().empty());
}

}  // namespace
