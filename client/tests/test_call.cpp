// Calls on loopback, between real librats nodes: each side sending and
// playing, a camera beside the screen, three in a call — everyone's voice and
// camera, the screen of whom one watches — a room where everyone meets
// everyone, hanging up, streams withdrawn and offered again, sources that run
// out, the monitor's reports, and the invites a call hands out.

#include "ratcast/client/call.h"
#include "ratcast/client/call_monitor.h"
#include "ratcast/client/camera.h"
#include "ratcast/client/screen_share.h"
#include "ratcast/client/voice.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/tone_source.h"
#include "ratcast/engine/virtual_output.h"

#include "udp_path.h"

#include "librats/subsystems/hole_punch.h"
#include "librats/subsystems/relay.h"
#include "librats/subsystems/rendezvous.h"

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
        .built_in_servers = {},
        .protocol = std::string("ratcast-test/") + test->test_suite_name() + "." + test->name(),
        .watch_network = false,
    };
}

/// A member of a room on loopback.
CallConfig room(std::optional<RatsUrl> dial = std::nullopt) {
    CallConfig config = ::config(std::move(dial));
    config.mesh = true;
    return config;
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
            config.playback.cameras = [this](const PeerId& peer) -> VideoPlayer::Sink {
                {
                    std::lock_guard lock(mutex_);
                    cameras_asked_.push_back(peer);
                }
                return [this](VideoFrame) {
                    std::lock_guard lock(mutex_);
                    ++camera_pictures_;
                };
            };
        }
        config.playback.sound = &speaker_;
        config.playback.heard = [this](const PeerId& peer, Timestamp, Timestamp) {
            std::lock_guard lock(mutex_);
            ++heard_;
            ++heard_from_[peer];
        };
        Result<std::unique_ptr<Call>> started = Call::start(std::move(config), handlers());
        ASSERT_TRUE(started) << to_string(started.error());
        call_ = std::move(*started);
    }

    void publish_pattern(uint32_t width = 320, uint32_t height = 240) {
        VideoPublisherConfig video;
        video.label = "pattern";
        video.encoder.bitrate = 1'000'000;
        video.start_bitrate = 1'000'000;
        ASSERT_TRUE(call_->publish(
            std::make_unique<PatternSource>(PatternConfig{PixelFormat::NV12, width, height, 30, 1}),
            video));
    }

    void publish_voice() {
        ASSERT_TRUE(ratcast::publish_voice(
            *call_, std::make_unique<ToneSource>(ToneConfig{.level = 0.5f})));
    }

    /// A voice and a camera, as one in a call has.
    void publish_voice_and_camera() {
        publish_voice();
        ASSERT_TRUE(publish_camera(*call_, std::make_unique<PatternSource>(
                                               PatternConfig{PixelFormat::NV12, 160, 120, 30, 1})));
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
    size_t heard_from(const PeerId& peer) {
        std::lock_guard lock(mutex_);
        const auto it = heard_from_.find(peer);
        return it != heard_from_.end() ? it->second : 0;
    }
    /// The loudest sample the speaker plays over `span`, from a little after
    /// now: what was pulled before is played out first.
    float loudest(std::chrono::milliseconds span = 200ms) {
        std::this_thread::sleep_for(50ms);
        loudest_ = 0.0f;
        std::this_thread::sleep_for(span);
        return loudest_;
    }
    size_t camera_pictures() {
        std::lock_guard lock(mutex_);
        return camera_pictures_;
    }
    /// The peers whose cameras it was asked where to show, in order.
    std::vector<PeerId> cameras_asked() {
        std::lock_guard lock(mutex_);
        return cameras_asked_;
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
            .lost = [this](const PeerId&) { tell("lost"); },
            .playing =
                [this](const PeerId&, const StreamDescription& stream, Result<void> outcome) {
                    tell((outcome ? "playing " : "failed ") + stream.label);
                },
            .ended = [this](const PeerId&,
                            const StreamDescription& stream) { tell("ended " + stream.label); },
            .presence =
                [this](const PeerId&, const Presence& presence) {
                    tell("presence " + presence.name + (presence.muted ? " muted" : ""));
                },
            .said =
                [this](const ChatMessage& message) {
                    tell("said " + message.name + ": " + message.text);
                },
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
    std::vector<PeerId> cameras_asked_;
    size_t heard_ = 0;
    std::map<PeerId, size_t> heard_from_;
    std::vector<std::string> events_;
    std::atomic<float> loudest_{0.0f};
    VirtualOutput speaker_{
        VirtualOutputConfig{}, [this](std::span<const float> samples, Timestamp) {
            float block = 0.0f;
            for (const float sample : samples) {
                block = std::max(block, std::abs(sample));
            }
            float most = loudest_.load();
            while (block > most && !loudest_.compare_exchange_weak(most, block)) {}
        }};
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

TEST_F(CallTest, AMonitorReportsWhatGoesEachWay) {
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
    ASSERT_TRUE(wait_for([&] {
        const auto path = guest->path(host->id());
        return guest.pictures() >= 10 && guest.heard() >= 20 && path && path->valid;
    }));

    CallMonitor sending(*host);
    CallMonitor playing(*guest);
    const size_t before = guest.pictures();
    ASSERT_TRUE(wait_for([&] { return guest.pictures() >= before + 15; }));
    const CallReport sent = sending.report();
    const CallReport played = playing.report();

    // The host sends the pattern and the tone, by stream id, to one viewer.
    ASSERT_EQ(sent.sent.size(), 2u);
    const SentStreamReport& pattern = sent.sent[0];
    EXPECT_EQ(pattern.label, "pattern");
    EXPECT_EQ(pattern.kind, MediaKind::Video);
    EXPECT_EQ(pattern.width, 320u);
    EXPECT_EQ(pattern.viewers, 1u);
    EXPECT_GT(pattern.fps, 0);
    EXPECT_GT(pattern.bitrate, 0);
    EXPECT_FALSE(pattern.encoder.empty());
    EXPECT_EQ(sent.sent[1].label, "tone");
    EXPECT_GT(sent.sent[1].fps, 0);
    EXPECT_FALSE(sent.video);

    // The guest plays them, over its one path.
    EXPECT_TRUE(played.sent.empty());
    ASSERT_TRUE(played.video);
    EXPECT_EQ(played.video->width, 320u);
    EXPECT_GT(played.video->fps, 0);
    EXPECT_GT(played.video->bitrate, 0);
    EXPECT_GT(played.video->pace.intervals().count(), 0u);
    ASSERT_TRUE(played.sound);
    EXPECT_GT(played.sound->fps, 0);
    ASSERT_EQ(played.paths.size(), 1u);
    EXPECT_EQ(played.paths.front().peer, host->id());
    EXPECT_GT(played.paths.front().rtt, Duration{});
    const std::vector<std::string> lines = report_lines(played);
    EXPECT_EQ(lines.front().rfind("pictures", 0), 0u) << lines.front();
    EXPECT_EQ(lines.back().rfind("call", 0), 0u) << lines.back();
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

    // Stopped, and the call goes on.
    unpublish_screen_share(*host);
    EXPECT_TRUE(wait_for([&] { return guest.told("ended window") && guest.told("ended sound"); }));
    EXPECT_FALSE(host->session().outgoing_stats(kScreenStream));
    EXPECT_FALSE(host->session().outgoing_stats(kScreenSoundStream));
    EXPECT_EQ(guest->peers().size(), 1u);
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
    ASSERT_EQ(guest->camera_stats().size(), 1u);
    EXPECT_EQ(guest->video_stats()->player.width, 320u);
    EXPECT_EQ(guest->camera_stats().at(host->id()).player.width, 160u);
    EXPECT_GT(previewed.load(), 0u) << "the sender sees its camera";
    const std::optional<OutgoingStreamStats> camera = host->session().outgoing_stats(3);
    ASSERT_TRUE(camera);
    EXPECT_LE(camera->bitrate, 1'000'000u);

    // The camera off, the screen goes on.
    host->unpublish(3);
    ASSERT_TRUE(wait_for([&] { return guest.told("ended camera"); }));
    EXPECT_TRUE(guest->camera_stats().empty());
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

// ── Three in a call ─────────────────────────────────────────────────────────

TEST_F(CallTest, EveryonesVoiceAndCameraPlay) {
    VideoEncoderConfig probe;
    probe.width = 160;
    probe.height = 120;
    if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

    Side host;
    host.start(config());
    Side ann;
    ann.start(config(RatsUrl{host.address()}));
    ann.publish_voice_and_camera();
    Side bob;
    bob.start(config(RatsUrl{host.address()}));
    bob.publish_voice_and_camera();

    // The host hears both and sees both, each apart.
    ASSERT_TRUE(wait_for([&] {
        return host->voice_stats().size() == 2 && host->camera_stats().size() == 2 &&
               host.camera_pictures() >= 20;
    })) << ::testing::PrintToString(host.events());
    EXPECT_EQ(host.told_times("playing voice"), 2u);
    EXPECT_EQ(host.told_times("playing camera"), 2u);
    std::vector<PeerId> asked = host.cameras_asked();
    std::sort(asked.begin(), asked.end());
    std::vector<PeerId> both{ann->id(), bob->id()};
    std::sort(both.begin(), both.end());
    EXPECT_EQ(asked, both) << "each camera to a place of its own";
    const size_t from_ann = host.heard_from(ann->id());
    const size_t from_bob = host.heard_from(bob->id());
    EXPECT_TRUE(wait_for([&] {
        return host.heard_from(ann->id()) >= from_ann + 25 &&
               host.heard_from(bob->id()) >= from_bob + 25;
    })) << "both mixed, playing, each told by whose";
    EXPECT_TRUE(wait_for([&] { return host->speaking().size() == 2; })) << "both heard speaking";
    const CallReport report = CallMonitor(*host).report();
    EXPECT_EQ(report.voices.size(), 2u);
    EXPECT_EQ(report.cameras.size(), 2u);

    // One leaves: the other is still heard and seen.
    bob.hang_up();
    ASSERT_TRUE(wait_for(
        [&] { return host->voice_stats().size() == 1 && host->camera_stats().size() == 1; }));
    EXPECT_TRUE(host->voice_stats().contains(ann->id()));
    EXPECT_TRUE(host->camera_stats().contains(ann->id()));
    EXPECT_EQ(host.told_times("ended voice"), 1u);
    EXPECT_EQ(host->speaking(), std::vector{ann->id()});
}

TEST_F(CallTest, OnlyTheCamerasOfTheLastToSpeakPlayWhenSoFewAre) {
    VideoEncoderConfig probe;
    probe.width = 160;
    probe.height = 120;
    if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

    Side host;
    CallConfig one_camera = room();
    one_camera.playback.most_cameras = 1;
    host.start(one_camera, false);
    Side ann;
    ann.start(room(RatsUrl{host.address()}), false, false);
    Side bob;
    bob.start(room(RatsUrl{host.address()}), false, false);
    const auto camera = [](Side& side) {
        ASSERT_TRUE(publish_camera(*side, std::make_unique<PatternSource>(
                                              PatternConfig{PixelFormat::NV12, 160, 120, 30, 1})));
    };
    const auto receives = [&](Side& from) {
        const std::optional<IncomingStreamStats> stats =
            host->session().incoming_stats(from->id(), kCameraStream);
        return stats && stats->subscribed;
    };

    // Nobody spoke: the first offered plays, the other is not received.
    camera(ann);
    ASSERT_TRUE(wait_for([&] { return host->cameras() == std::vector{ann->id()}; }));
    camera(bob);
    ASSERT_TRUE(wait_for([&] { return host->session().offered().size() == 2; }));
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(host->cameras(), std::vector{ann->id()}) << "one that never spoke takes no place";
    EXPECT_FALSE(receives(bob));

    // Bob speaks: his camera in Ann's place.
    bob.publish_voice();
    ASSERT_TRUE(wait_for([&] { return host->cameras() == std::vector{bob->id()}; }));
    EXPECT_TRUE(receives(bob));
    // Ann's player is stopped — and unsubscribed — just after the choice,
    // outside the call's lock.
    EXPECT_TRUE(wait_for([&] { return !receives(ann); })) << "let go";
    EXPECT_TRUE(wait_for([&] {
        return host.told_times("ended camera") == 1 && host.told_times("playing camera") == 2;
    })) << ::testing::PrintToString(host.events());
    const size_t pictures = host.camera_pictures();
    EXPECT_TRUE(wait_for([&] { return host.camera_pictures() >= pictures + 10; }));

    // Ann speaks too, and Bob stops: Ann's again.
    ann.publish_voice();
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(host->cameras(), std::vector{bob->id()}) << "heard at once: kept";
    bob->unpublish(4);
    EXPECT_TRUE(wait_for([&] { return host->cameras() == std::vector{ann->id()}; }));
}

TEST_F(CallTest, EachVoicePlaysAsLoudAsThisSideSets) {
    Side host;
    host.start(config(), false, false);
    Side ann;
    ann.start(config(RatsUrl{host.address()}), false, false);
    ASSERT_TRUE(wait_for([&] { return host.told("joined"); }));

    // Set before the voice plays: it starts silent, yet heard speaking.
    host->set_volume(ann->id(), 0.0f);
    EXPECT_EQ(host->volume(ann->id()), 0.0f);
    ann.publish_voice();
    ASSERT_TRUE(wait_for([&] { return host.heard_from(ann->id()) >= 25; }));
    EXPECT_TRUE(wait_for([&] { return host->speaking() == std::vector{ann->id()}; }));
    EXPECT_EQ(host.loudest(), 0.0f);

    // Turned up and down as it plays; the tone is at half of full scale.
    host->set_volume(ann->id(), 1.0f);
    EXPECT_EQ(host->volume(ann->id()), 1.0f);
    EXPECT_GT(host.loudest(), 0.35f);
    host->set_volume(ann->id(), 0.25f);
    const float quarter = host.loudest();
    EXPECT_GT(quarter, 0.07f);
    EXPECT_LT(quarter, 0.2f);
    EXPECT_EQ(host->volume(host->id()), 1.0f) << "anyone else's as it comes";
}

TEST_F(CallTest, TheSoundWatchedPlaysAsLoudAsThisSideSetsApartFromTheVoices) {
    Side host;
    host.start(config(), false, false);
    Side guest;
    guest.start(config(RatsUrl{host.address()}), false, false);

    // Set before the sound plays: it starts silent, yet played.
    guest->set_share_volume(0.0f);
    EXPECT_EQ(guest->share_volume(), 0.0f);
    host.publish_tone();
    ASSERT_TRUE(wait_for([&] { return guest.told("playing tone") && guest.heard() >= 25; }));
    EXPECT_EQ(guest.loudest(), 0.0f);

    guest->set_share_volume(1.0f);
    EXPECT_GT(guest.loudest(), 0.35f);
    guest->set_share_volume(0.25f);
    const float quarter = guest.loudest();
    EXPECT_GT(quarter, 0.07f);
    EXPECT_LT(quarter, 0.2f);

    // The voices as they come, whatever the share's.
    guest->set_share_volume(0.0f);
    host.publish_voice();
    ASSERT_TRUE(wait_for([&] { return guest.heard_from(host->id()) >= 50; }));
    EXPECT_GT(guest.loudest(), 0.35f);
}

TEST_F(CallTest, PlaysTheScreenOfWhomItWatchesAndReceivesNoOther) {
    VideoEncoderConfig probe;
    probe.width = 160;
    probe.height = 120;
    if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

    Side host;
    host.start(config());
    Side ann;
    ann.start(config(RatsUrl{host.address()}));
    ann.publish_pattern(320, 240);
    ann.publish_tone();
    Side bob;
    bob.start(config(RatsUrl{host.address()}));
    bob.publish_pattern(160, 120);
    bob.publish_tone();
    const StreamId pattern = VideoPublisherConfig{}.stream;
    const auto received = [&](Side& from, StreamId stream) {
        const std::optional<IncomingStreamStats> stats =
            host->session().incoming_stats(from->id(), stream);
        return stats && stats->subscribed;
    };
    const auto watching = [&](Side& whom, uint32_t width) {
        const std::optional<ReceivedVideoStats> video = host->video_stats();
        return video && video->peer == whom->id() && video->player.width == width &&
               host->sound_stats() && received(whom, pattern);
    };

    // Nothing chosen: the first offered plays, the other is not received.
    ASSERT_TRUE(wait_for([&] {
        return host->video_stats() && host->sound_stats() && host->session().offered().size() == 4;
    }));
    const bool ann_first = host->video_stats()->peer == ann->id();
    Side& first = ann_first ? ann : bob;
    Side& other = ann_first ? bob : ann;
    const uint32_t other_width = ann_first ? 160 : 320;
    EXPECT_FALSE(received(other, pattern));

    // The other chosen: its pictures and sound play in place of the first's.
    host->watch(other->id());
    ASSERT_TRUE(wait_for([&] { return watching(other, other_width); }));
    EXPECT_TRUE(wait_for([&] { return !received(first, pattern); })) << "the first let go";
    EXPECT_EQ(host.told_times("ended pattern"), 1u);
    EXPECT_EQ(host.told_times("ended tone"), 1u);
    const size_t pictures = host.pictures();
    EXPECT_TRUE(wait_for([&] { return host.pictures() >= pictures + 10; }));

    // Nobody: nothing plays, nothing comes.
    host->watch(std::nullopt);
    EXPECT_FALSE(host->video_stats());
    EXPECT_FALSE(host->sound_stats());
    EXPECT_TRUE(wait_for([&] { return !received(ann, pattern) && !received(bob, pattern); }));

    // Chosen, its share ending leaves nothing playing: another's is not taken.
    host->watch(first->id());
    ASSERT_TRUE(wait_for([&] { return watching(first, ann_first ? 320 : 160); }));
    first->unpublish(pattern);
    ASSERT_TRUE(wait_for([&] { return !host->video_stats(); }));
    std::this_thread::sleep_for(200ms);  // as long as playing the other's would take
    EXPECT_FALSE(host->video_stats());
    EXPECT_TRUE(host->sound_stats()) << "the first's sound goes on";
}

TEST_F(CallTest, TheViewersOfAShareAllPlayItAtOnce) {
    VideoEncoderConfig probe;
    probe.width = 160;
    probe.height = 120;
    if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

    Side host;
    host.start(config());
    host.publish_pattern(160, 120);
    host.publish_tone();
    Side ann;
    ann.start(config(RatsUrl{host.address()}));
    Side bob;
    bob.start(config(RatsUrl{host.address()}));

    // Each measures its clock against the host's and says what it needs; the
    // host tells both the most of it.
    const auto together = [](Side& viewer) { return viewer->sync_stats().together; };
    ASSERT_TRUE(wait_for([&] {
        return together(ann) && together(bob) && together(ann) == together(bob);
    })) << ::testing::PrintToString(ann.events());
    for (Side* viewer : {&ann, &bob}) {
        const PlayoutClockStats sync = (*viewer)->sync_stats();
        ASSERT_TRUE(sync.need);
        EXPECT_GE(*sync.together, *sync.need - 5ms) << "covers what each needs, give or take "
                                                       "a clock measured over loopback";
    }

    // One leaves: nobody to be in step with.
    bob.hang_up();
    EXPECT_TRUE(wait_for([&] { return !together(ann); }));
    const size_t pictures = ann.pictures();
    EXPECT_TRUE(wait_for([&] { return ann.pictures() >= pictures + 10; })) << "and plays on";
}

TEST_F(CallTest, WithNothingChosenTheNextOfferedPlaysWhenTheScreenEnds) {
    VideoEncoderConfig probe;
    probe.width = 160;
    probe.height = 120;
    if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

    Side host;
    host.start(config());
    Side ann;
    ann.start(config(RatsUrl{host.address()}));
    ann.publish_pattern(160, 120);
    Side bob;
    bob.start(config(RatsUrl{host.address()}));
    bob.publish_pattern(160, 120);
    ASSERT_TRUE(
        wait_for([&] { return host->video_stats() && host->session().offered().size() == 2; }));
    const PeerId first = host->video_stats()->peer;
    Side& sharer = first == ann->id() ? ann : bob;
    Side& next = first == ann->id() ? bob : ann;

    sharer->unpublish(VideoPublisherConfig{}.stream);
    ASSERT_TRUE(wait_for([&] {
        const std::optional<ReceivedVideoStats> video = host->video_stats();
        return video && video->peer == next->id() && video->player.pictures > 0;
    })) << ::testing::PrintToString(host.events());
    EXPECT_EQ(host.told_times("playing pattern"), 2u);
}

// ── A room ──────────────────────────────────────────────────────────────────

TEST_F(CallTest, EachSaysWhoItIs) {
    Side host;
    host.start(room(), false, false);
    ASSERT_TRUE(host->set_presence({"Ann"}));  // before anyone is there
    Side guest;
    guest.start(room(RatsUrl{host.address()}), false, false);
    ASSERT_TRUE(wait_for([&] { return guest.told("presence Ann"); }));
    EXPECT_EQ(guest->presence(host->id()), Presence{"Ann"});
    EXPECT_FALSE(host->presence(guest->id())) << "the guest said nothing yet";

    ASSERT_TRUE(guest->set_presence({"Bob", true, false}));  // once there
    ASSERT_TRUE(wait_for([&] { return host.told("presence Bob muted"); }));
    EXPECT_EQ(host->presence(guest->id()), (Presence{"Bob", true, false}));
    EXPECT_EQ(host->set_presence({std::string(Presence::kMaxNameSize + 1, 'x')}).error(),
              Errc::InvalidArgument);
}

TEST_F(CallTest, InARoomEveryoneMeetsEveryoneWhoeverTheyJoinThrough) {
    Side host;
    host.start(room(), /*pictures=*/false, /*camera=*/false);
    // Two join through the host at once, a third through one of them.
    Side ann;
    Side bob;
    ann.start(room(RatsUrl{host.address()}), false, false);
    bob.start(room(RatsUrl{host.address()}), false, false);
    Side carl;
    carl.start(room(RatsUrl{ann.address()}), false, false);
    const std::vector<Side*> members{&host, &ann, &bob, &carl};
    for (Side* member : members) member->publish_voice();
    const auto all_met = [&] {
        return std::ranges::all_of(members, [&](Side* member) {
            return (*member)->peers().size() == members.size() - 1 &&
                   (*member)->voice_stats().size() == members.size() - 1;
        });
    };
    const auto state = [&] {
        std::string text;
        for (Side* member : members) {
            text += std::to_string((*member)->peers().size()) + " peers, " +
                    std::to_string((*member)->voice_stats().size()) + " voices; " +
                    ::testing::PrintToString(member->events()) + "\n";
        }
        return text;
    };
    ASSERT_TRUE(wait_for(all_met)) << state();

    // One leaves, and comes back through another: everyone meets again.
    bob.hang_up();
    ASSERT_TRUE(wait_for([&] {
        return host->peers().size() == 2 && ann->peers().size() == 2 && carl->peers().size() == 2 &&
               host->voice_stats().size() == 2;
    }));
    bob.start(room(RatsUrl{carl.address()}), false, false);
    bob.publish_voice();
    EXPECT_TRUE(wait_for(all_met));
}

/// What a ratcast-server is to a call: a node it meets the others at, which
/// carries what no punch gets through.
class StandInServer {
public:
    StandInServer() : node_(node_config()) {
        librats::Rendezvous::Config meeting;
        meeting.serve = true;
        node_.add_subsystem(std::make_unique<librats::Rendezvous>(meeting));
        node_.add_subsystem(std::make_unique<librats::HolePunch>());
        librats::Relay::Config relay;
        relay.serve = true;
        node_.add_subsystem(std::make_unique<librats::Relay>(relay));
        EXPECT_TRUE(node_.start());
    }
    ~StandInServer() { node_.stop(); }

    /// Know `other`: each says Hello, a server the other's calls are told of.
    void meet(const StandInServer& other) {
        node_.connect("127.0.0.1", other.node_.listen_port());
        EXPECT_TRUE(wait_for([&] { return node_.peer(other.id()).has_value(); }));
    }

    const PeerId& id() const { return node_.local_id(); }
    librats::HostEndpoint address() const { return {"127.0.0.1", node_.listen_port()}; }

private:
    static librats::NodeConfig node_config() {
        librats::NodeConfig node;
        node.bind_address = "127.0.0.1";
        node.protocol = config().protocol;
        return node;
    }

    librats::Node node_;
};

/// A call met by peer id at `server`, and there alone.
CallConfig at_server(const StandInServer& server, std::optional<RatsUrl> dial = std::nullopt) {
    CallConfig config = ::config(std::move(dial));
    config.discoverable = !config.dial;
    config.dht = false;
    config.servers = {server.address()};
    return config;
}

/// A room's member whose links go lost after `lost_after` of silence, and
/// whose lost peers are dialed again for `redial_for`.
CallConfig quick_to_lose(std::optional<RatsUrl> dial = std::nullopt,
                         Duration lost_after = std::chrono::seconds(4),
                         Duration redial_for = std::chrono::seconds(30)) {
    CallConfig config = room(std::move(dial));
    config.lost_after = lost_after;
    config.redial_for = redial_for;
    return config;
}

/// Ann and Bob in one room, each knowing the other's name.
void meet(Side& ann, Side& bob) {
    ASSERT_TRUE(ann->set_presence({.name = "Ann"}));
    ASSERT_TRUE(bob->set_presence({.name = "Bob"}));
    ASSERT_TRUE(wait_for([&] {
        const std::optional<Presence> bob_said = ann->presence(bob->id());
        return ann->peers().size() == 1 && bob_said && bob_said->name == "Bob";
    }));
}

TEST_F(CallTest, AMemberWhoseNetworkGoesIsAwayAndComesBackByItself) {
    Side ann;
    ann.start(quick_to_lose(), false, false);
    test::UdpPath path(ann->port());
    Side bob;
    bob.start(quick_to_lose(RatsUrl{librats::HostEndpoint{"127.0.0.1", path.port()}}), false,
              false);
    meet(ann, bob);
    ann.publish_voice();  // something Bob answers, even silent
    EXPECT_FALSE(ann->interrupted(bob->id()));

    // Gone quiet: cut off, not yet lost.
    path.dead = true;
    EXPECT_TRUE(wait_for([&] { return ann->interrupted(bob->id(), std::chrono::seconds(1)); },
                         std::chrono::seconds(3)));
    EXPECT_EQ(ann->peers().size(), 1u) << "kept while it may come back";

    // Lost: away, still known by name, dialed again where it said it listens
    // — here, past the dead path — and back.
    ASSERT_TRUE(wait_for([&] { return ann.told("lost"); }));
    EXPECT_TRUE(wait_for([&] { return ann.told_times("joined") == 2; }))
        << ::testing::PrintToString(ann.events());
    EXPECT_TRUE(ann->away().empty());
    EXPECT_FALSE(ann->interrupted(bob->id()));
    EXPECT_FALSE(ann.told("left")) << "never gone";
    ASSERT_TRUE(ann->presence(bob->id())) << "known by name throughout";
    EXPECT_EQ(ann->presence(bob->id())->name, "Bob");
}

TEST_F(CallTest, AMemberLostWhileAwayIsShownAwayByNameAndLeftInTime) {
    Side ann;
    ann.start(quick_to_lose(std::nullopt, std::chrono::milliseconds(1500), std::chrono::seconds(2)),
              false, false);
    test::UdpPath path(ann->port());
    Side bob;
    bob.start(quick_to_lose(RatsUrl{librats::HostEndpoint{"127.0.0.1", path.port()}}), false,
              false);
    meet(ann, bob);
    const PeerId bob_id = bob->id();

    // Gone with the network down: the goodbye never comes.
    path.dead = true;
    bob.hang_up();
    ASSERT_TRUE(wait_for([&] { return ann.told("lost"); }));
    EXPECT_EQ(ann->away(), std::vector{bob_id});
    EXPECT_TRUE(ann->peers().empty());
    EXPECT_TRUE(ann->interrupted(bob_id));
    ASSERT_TRUE(ann->presence(bob_id));
    EXPECT_EQ(ann->presence(bob_id)->name, "Bob") << "known by name while away";

    EXPECT_TRUE(wait_for([&] { return ann.told("left"); })) << "given up on in time";
    EXPECT_TRUE(ann->away().empty());
    EXPECT_FALSE(ann->presence(bob_id));
}

TEST_F(CallTest, AMemberWhoHangsUpIsLeftAtOnce) {
    Side ann;
    ann.start(quick_to_lose(), false, false);
    Side bob;
    bob.start(quick_to_lose(RatsUrl{ann.address()}), false, false);
    ASSERT_TRUE(wait_for([&] { return ann->peers().size() == 1; }));
    bob.hang_up();
    EXPECT_TRUE(wait_for([&] { return ann.told("left"); }, std::chrono::seconds(2)));
    EXPECT_FALSE(ann.told("lost")) << "a goodbye is no loss";
    EXPECT_TRUE(ann->away().empty());
}

TEST_F(CallTest, WhatIsSaidReachesEveryMemberAndOneWhoComesLaterReadsIt) {
    Side host;
    host.start(room(), false, false);
    Side ann;
    ann.start(room(RatsUrl{host.address()}), false, false);
    meet(host, ann);  // named Ann and Bob
    ASSERT_TRUE(host->set_presence({.name = "Host"}));
    ASSERT_TRUE(ann->set_presence({.name = "Ann"}));

    const Result<ChatMessage> hi = ann->say("hi, all");
    ASSERT_TRUE(hi);
    EXPECT_EQ(hi->name, "Ann");
    EXPECT_EQ(hi->author, ann->id());
    ASSERT_TRUE(wait_for([&] { return host.told("said Ann: hi, all"); }));
    ASSERT_TRUE(host->say("welcome"));
    ASSERT_TRUE(wait_for([&] { return ann.told("said Host: welcome"); }));

    // Bob comes later, through Ann, and reads it all — once.
    Side bob;
    bob.start(room(RatsUrl{ann.address()}), false, false);
    ASSERT_TRUE(wait_for([&] { return bob->peers().size() == 2; }));
    ASSERT_TRUE(wait_for([&] { return bob->chat().size() == 2; }));
    EXPECT_EQ(bob->chat(), host->chat());
    EXPECT_EQ(bob->chat(), ann->chat());
    ASSERT_TRUE(bob->set_presence({.name = "Bob"}));
    ASSERT_TRUE(bob->say("sorry I'm late"));
    for (Side* other : {&host, &ann}) {
        EXPECT_TRUE(wait_for([&] { return other->told("said Bob: sorry I'm late"); }));
    }
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(bob.told_times("said Ann: hi, all"), 1u);
    EXPECT_EQ(host->say("").error(), Errc::InvalidArgument);
}

TEST_F(CallTest, WhatWasSaidWhileAMemberWasAwayIsToldItOnItsReturn) {
    Side ann;
    ann.start(quick_to_lose(std::nullopt, std::chrono::milliseconds(1500)), false, false);
    test::UdpPath path(ann->port());
    Side bob;
    bob.start(quick_to_lose(RatsUrl{librats::HostEndpoint{"127.0.0.1", path.port()}},
                            std::chrono::milliseconds(1500)),
              false, false);
    meet(ann, bob);

    path.dead = true;
    ASSERT_TRUE(wait_for([&] { return ann.told("lost"); }));
    ASSERT_TRUE(ann->say("are you there?"));
    path.dead = false;
    ASSERT_TRUE(wait_for([&] { return bob.told("said Ann: are you there?"); }))
        << ::testing::PrintToString(bob.events());
    EXPECT_EQ(bob->chat(), ann->chat());
}

TEST_F(CallTest, CallsMeetByPeerIdAtAServerWhichIsNoMember) {
    StandInServer server;
    Side host;
    host.start(at_server(server), /*pictures=*/false, /*camera=*/false);
    host.publish_voice();
    Side guest;
    guest.start(at_server(server, RatsUrl{host->id()}), false, false);

    ASSERT_TRUE(wait_for([&] { return guest.heard_from(host->id()) > 0; }))
        << ::testing::PrintToString(guest.events());
    EXPECT_EQ(host->peers(), std::vector<PeerId>{guest->id()});
    EXPECT_EQ(guest->peers(), std::vector<PeerId>{host->id()});
    EXPECT_EQ(guest.told_times("joined"), 1u) << "the server was taken for a member";
    EXPECT_EQ(host.told_times("joined"), 1u);
    EXPECT_FALSE(guest.told("left"));
    EXPECT_EQ(host->servers(), std::vector<PeerId>{server.id()});
}

// A room's member that knows no server meets at the one another member names:
// the server, met as a member for a moment, is said to have left.
TEST_F(CallTest, AServerAMemberNamesIsNoMemberEither) {
    StandInServer server;
    Side host;
    CallConfig hosting = room();
    hosting.discoverable = true;
    hosting.dht = false;
    hosting.servers = {server.address()};
    host.start(hosting, false, false);
    // Met before the guest asks the host whom it knows: asked once, as it
    // connects, the host names only the peers it has then.
    ASSERT_TRUE(wait_for([&] { return host->servers() == std::vector<PeerId>{server.id()}; }));
    EXPECT_TRUE(host->peers().empty());
    EXPECT_FALSE(host.told("joined"));
    Side guest;
    CallConfig joining = room(RatsUrl{host.address()});
    guest.start(joining, false, false);

    ASSERT_TRUE(wait_for([&] { return guest->peers() == std::vector<PeerId>{host->id()}; }));
    // The host's peer exchange named the server; met, it said Hello.
    ASSERT_TRUE(wait_for([&] { return guest->servers() == std::vector<PeerId>{server.id()}; }));
    EXPECT_EQ(guest->peers(), std::vector<PeerId>{host->id()});
    EXPECT_TRUE(wait_for([&] { return guest.told_times("left") == 1; }));
    EXPECT_EQ(guest.told_times("joined"), 2u);
}

// The server a call meets at tells it of the others it knows; the call keeps
// them in its data directory, and the next one there knows them from the start.
TEST_F(CallTest, ACallKeepsTheServersItIsToldOfForTheNext) {
    StandInServer server, other;
    server.meet(other);
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "ratcast_call_known_servers";
    std::filesystem::remove_all(folder);
    const auto knows = [](Side& side, const librats::HostEndpoint& at) {
        const std::vector<librats::HostEndpoint> known = side->known_servers();
        return std::ranges::find(known, at) != known.end();
    };
    const auto knows_other = [&](Side& side) { return knows(side, other.address()); };

    Side first;
    CallConfig config = at_server(server);
    config.data_dir = folder.string();
    first.start(config, false, false);
    EXPECT_TRUE(wait_for([&] { return knows_other(first); }));
    EXPECT_FALSE(knows(first, server.address())) << "told of the server it is at";
    first.hang_up();

    Side next;
    CallConfig alone = config;
    alone.servers = {};
    next.start(alone, false, false);
    EXPECT_TRUE(knows_other(next));
    next.hang_up();
    std::filesystem::remove_all(folder);
}

// Two that named no server meet at one of those they know, chosen alike by
// the call's key though their lists differ. On loopback every server is one
// host, whose first given is the one chosen.
TEST_F(CallTest, CallsMeetAtAServerTheyChoseOfThoseTheyKnow) {
    StandInServer only_host, both, only_guest;
    const auto knowing = [](CallConfig config, std::vector<librats::HostEndpoint> servers) {
        config.dht = false;
        config.chosen_servers = 2;
        config.built_in_servers = std::move(servers);
        return config;
    };
    Side host;
    CallConfig hosting = config();
    hosting.discoverable = true;
    host.start(knowing(hosting, {both.address(), only_host.address()}), false, false);
    host.publish_voice();
    Side guest;
    guest.start(knowing(config(RatsUrl{host->id()}), {both.address(), only_guest.address()}), false,
                false);

    ASSERT_TRUE(wait_for([&] { return guest.heard_from(host->id()) > 0; }))
        << ::testing::PrintToString(guest.events());
    EXPECT_EQ(host->servers(), std::vector<PeerId>{both.id()});
    EXPECT_EQ(guest->servers(), std::vector<PeerId>{both.id()});
    EXPECT_EQ(guest->peers(), std::vector<PeerId>{host->id()});
}

// A room's link names the servers its members meet at, which whoever joins by
// it meets at too, and names in its own.
TEST_F(CallTest, AnInviteNamesTheServersTheRoomIsMetAt) {
    StandInServer server;
    Side host;
    CallConfig hosting = room();
    hosting.discoverable = true;
    hosting.dht = false;
    hosting.servers = {server.address()};
    host.start(hosting, false, false);
    const std::vector<std::string> invites = host->invites();
    ASSERT_FALSE(invites.empty());
    const std::optional<RatsLink> link = parse_rats_link(invites.front());
    ASSERT_TRUE(link);
    EXPECT_EQ(link->url, RatsUrl{host->id()});
    EXPECT_EQ(link->via, std::vector{server.address()});

    Side guest;
    CallConfig joining = room(link->url);
    joining.dht = false;
    joining.servers = link->via;
    guest.start(joining, false, false);
    ASSERT_TRUE(wait_for([&] { return guest->peers() == std::vector<PeerId>{host->id()}; }));
    EXPECT_EQ(guest->invites().front(), invites.front());
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
