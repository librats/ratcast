// Calls on loopback, between real librats nodes: each side sending and
// playing, hanging up, streams withdrawn and offered again, sources that run
// out, and the invites a call hands out.

#include "ratcast/client/call.h"

#include <gtest/gtest.h>

#include <algorithm>
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

    void start(CallConfig config, bool pictures = true) {
        if (pictures) {
            config.playback.pictures = [this](VideoFrame) {
                std::lock_guard lock(mutex_);
                ++pictures_;
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
    // Two peers, one subscriber: the call that plays nothing never asked.
    EXPECT_TRUE(wait_for([&] { return host->peers().size() == 2; }));
    EXPECT_EQ(host->session().outgoing_stats(AudioPublisherConfig{}.stream)->subscribers, 1u);
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
