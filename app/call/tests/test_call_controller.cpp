// CallController in rooms on loopback: one makes a room and others join it by
// its invites or by each other's, each going through its states; everyone
// talking and seen speaking, muted, deafened; anyone sharing (the test
// pattern and a tone standing in for the desktop's screen and sound) and
// stopping while the room goes on, the others watching it or not; cameras,
// each member's in a view of its own; each listing the others by the names
// they give; each reporting how the call goes; and the ways a share or a
// join ends without the user.

#include "call_controller.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <thread>

#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/tone_source.h"
#include "ratcast/engine/virtual_output.h"

#include "librats/node/node.h"
#include "librats/subsystems/rendezvous.h"

using namespace ratcast;
using namespace ratcast::app;
using namespace std::chrono_literals;
using State = CallController::State;

namespace {

/// Run the event loop until `predicate` holds, or `timeout` passes.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

/// Pictures that run out: the pattern, a few frames of it, then the end.
class ShortPattern final : public VideoSource {
public:
    ~ShortPattern() override { stop(); }
    Result<void> start(Sink sink, EndHandler ended) override {
        thread_ = std::thread([sink = std::move(sink), ended = std::move(ended)] {
            const PatternConfig pattern{PixelFormat::NV12, 320, 240, 30, 1};
            for (uint64_t i = 0; i < 10; ++i) {
                sink(draw_pattern(pattern, i, SteadyClock::instance().now()));
                std::this_thread::sleep_for(33ms);
            }
            ended({});
        });
        return {};
    }
    void stop() override {
        if (thread_.joinable()) thread_.join();
    }

private:
    std::thread thread_;
};

/// Screens kept on, by every side: the system's left alone.
std::atomic<int> g_screens_kept{0};

class KeptAwake final : public ScreenAwake {
public:
    KeptAwake() { ++g_screens_kept; }
    ~KeptAwake() override { --g_screens_kept; }
    bool held() const noexcept override { return true; }
};

/// The test's own: loopback, its own protocol, nothing kept, a pattern for
/// the screen and a tone for its sound, another for each voice, a speaker
/// that plays to nobody.
CallController::Environment environment() {
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    CallController::Environment environment;
    environment.call.bind_address = "127.0.0.1";
    environment.call.protocol =
        std::string("ratcast-test/") + test->test_suite_name() + "." + test->name();
    environment.call.watch_network = false;
    environment.open_share = [](const ScreenShareConfig& config,
                                std::stop_token) -> Result<ScreenShareSources> {
        ScreenShareSources sources{.pictures = std::make_unique<PatternSource>(
                                       PatternConfig{PixelFormat::NV12, 320, 240, 30, 1})};
        if (config.sound) {
            sources.sound = std::make_unique<ToneSource>(ToneConfig{.format = {48'000, 2}});
        }
        return sources;
    };
    environment.open_speaker = [] {
        return std::make_unique<VirtualOutput>(VirtualOutputConfig{});
    };
    environment.open_microphone = [] {
        return std::make_unique<ToneSource>(ToneConfig{.frequency = 300, .level = 0.3f});
    };
    environment.open_camera = [] {
        return std::make_unique<PatternSource>(PatternConfig{PixelFormat::NV12, 160, 120, 30, 1});
    };
    environment.keep_screen_awake = [] { return std::make_unique<KeptAwake>(); };
    // Tones for voices: not for the echo canceller or the noise suppressor.
    environment.voice.processing.echo_cancellation = false;
    environment.voice.processing.noise_suppression = false;
    return environment;
}

class CallControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        VideoEncoderConfig probe;
        probe.width = 320;
        probe.height = 240;
        if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";
        if (!open_audio_encoder({})) GTEST_SKIP() << "no Opus here";
        for (CallController* side : {&ann_, &bob_, &carl_}) side->setEnvironment(environment());
        ann_.setName("Ann");
        bob_.setName("Bob");
        carl_.setName("Carl");
        bob_.setVideo(&bob_view_);
    }

    /// Ann's room made, and its invite on loopback.
    QString make_room() {
        ann_.create();
        EXPECT_EQ(ann_.state(), State::InRoom) << ann_.notice().toStdString();
        EXPECT_EQ(ann_.invites().size(), 1) << "one address: the one it listens on";
        return ann_.invites().value(0);
    }

    /// Bob in Ann's room, each knowing the other by name.
    void both_in() {
        bob_.join(make_room());
        ASSERT_TRUE(wait_for(
            [&] { return !member(ann_, "Bob").isEmpty() && !member(bob_, "Ann").isEmpty(); }));
    }

    static int members(const CallController& side) { return side.memberList().rowCount(); }
    /// The member of `side` called `name`; empty if none is.
    static QVariantMap member(const CallController& side, const QString& name) {
        for (int row = 0; row < members(side); ++row) {
            const QVariantMap member = side.memberList().get(row);
            if (member.value("name") == name) return member;
        }
        return {};
    }
    /// The id the others know `side` by, as `other` lists it.
    static QString id_of(const CallController& other, const QString& name) {
        return member(other, name).value("peerId").toString();
    }

    VideoView bob_view_;
    CallController ann_;
    CallController bob_;
    CallController carl_;
};

TEST_F(CallControllerTest, OneMakesARoomAndAnotherJoins) {
    const QString invite = make_room();
    EXPECT_TRUE(invite.startsWith("rats://127.0.0.1:"));
    EXPECT_EQ(members(ann_), 0) << "alone";

    bob_.join(invite);
    EXPECT_EQ(bob_.state(), State::Joining);
    ASSERT_TRUE(wait_for([&] { return bob_.state() == State::InRoom; }));
    EXPECT_TRUE(wait_for([&] { return members(ann_) == 1 && members(bob_) == 1; }));
    EXPECT_FALSE(bob_.invites().isEmpty()) << "any member's link will do";
    EXPECT_TRUE(bob_.notice().isEmpty());

    bob_.leave();
    EXPECT_EQ(bob_.state(), State::Idle);
    EXPECT_EQ(members(bob_), 0);
    EXPECT_TRUE(bob_.invites().isEmpty());
    EXPECT_TRUE(wait_for([&] { return members(ann_) == 0; }));
    EXPECT_EQ(ann_.state(), State::InRoom) << "still in it, for the next to join";
}

TEST_F(CallControllerTest, AThirdJoiningThroughTheSecondMeetsTheFirst) {
    both_in();
    carl_.join(bob_.invites().value(0));
    EXPECT_TRUE(wait_for([&] {
        return members(ann_) == 2 && members(bob_) == 2 && !member(carl_, "Ann").isEmpty() &&
               !member(carl_, "Bob").isEmpty();
    })) << members(ann_)
        << " " << members(bob_) << " " << members(carl_);
}

TEST_F(CallControllerTest, EachListsTheOthersByTheNamesTheyGive) {
    both_in();
    EXPECT_EQ(member(ann_, "Bob").value("peer").toString().size(), 8) << "a short id beside it";
    EXPECT_EQ(id_of(ann_, "Bob").size(), 64);

    bob_.setName(QString("Robert\n\tthe   ") + QString(200, QChar(0x044F)));  // 'я'
    ASSERT_TRUE(wait_for([&] { return member(ann_, "Bob").isEmpty() && members(ann_) == 1; }));
    const QString told = ann_.memberList().get(0).value("name").toString();
    EXPECT_TRUE(told.startsWith("Robert the яя")) << told.toStdString();
    EXPECT_LE(told.toUtf8().size(), qsizetype(Presence::kMaxNameSize));
}

TEST_F(CallControllerTest, EveryoneTalksAndIsSeenSpeakingUntilMutedOrDeafened) {
    both_in();
    const auto bob = [&] { return member(ann_, "Bob"); };
    ASSERT_TRUE(wait_for([&] {
        return bob().value("speaking").toBool() && member(bob_, "Ann").value("speaking").toBool();
    })) << "each hears the other's voice";
    EXPECT_TRUE(wait_for([&] { return ann_.speaking() && bob_.speaking(); }));

    bob_.setMuted(true);
    EXPECT_FALSE(bob_.speaking());
    EXPECT_TRUE(wait_for(
        [&] { return bob().value("muted").toBool() && !bob().value("speaking").toBool(); }));
    bob_.setMuted(false);
    EXPECT_TRUE(wait_for([&] { return bob().value("speaking").toBool(); }));

    // Deafened: hears nobody, and nobody hears it, whatever muted says.
    bob_.setDeafened(true);
    EXPECT_FALSE(bob_.muted()) << "the user's own choice, kept";
    EXPECT_TRUE(wait_for([&] {
        return bob().value("deafened").toBool() && bob().value("muted").toBool() &&
               !bob().value("speaking").toBool();
    }));
    bob_.setDeafened(false);
    EXPECT_TRUE(wait_for(
        [&] { return !bob().value("muted").toBool() && bob().value("speaking").toBool(); }));
}

TEST_F(CallControllerTest, WithoutAMicrophoneTheRoomGoesOn) {
    CallController::Environment mute = environment();
    mute.open_microphone = [] { return std::unique_ptr<AudioSource>{}; };
    bob_.setEnvironment(mute);
    bob_.setName("Bob");
    both_in();
    EXPECT_FALSE(bob_.notice().isEmpty());
    EXPECT_TRUE(wait_for([&] { return member(bob_, "Ann").value("speaking").toBool(); }));
    EXPECT_FALSE(member(ann_, "Bob").value("speaking").toBool());
}

TEST_F(CallControllerTest, AnyoneSharesAndStopsWhileTheRoomGoesOn) {
    both_in();
    ann_.share(false, true);
    EXPECT_TRUE(ann_.choosing());
    ASSERT_TRUE(wait_for([&] { return ann_.sharing(); })) << ann_.notice().toStdString();
    EXPECT_FALSE(ann_.choosing());

    // Nothing chosen: the first share offered is watched.
    const QString ann = id_of(bob_, "Ann");
    ASSERT_TRUE(wait_for([&] { return bob_.watching() == ann; }));
    EXPECT_TRUE(wait_for([&] { return bob_view_.pictureSize() == QSize(320, 240); }))
        << "the pictures reach the view";
    EXPECT_TRUE(member(bob_, "Ann").value("sharing").toBool());
    EXPECT_TRUE(ann_.watching().isEmpty()) << "not one's own";

    ann_.stopSharing();
    EXPECT_FALSE(ann_.sharing());
    EXPECT_TRUE(wait_for([&] { return bob_.watching().isEmpty(); }));
    EXPECT_TRUE(bob_view_.pictureSize().isEmpty()) << "the view cleared";
    EXPECT_TRUE(wait_for([&] { return !member(bob_, "Ann").value("sharing").toBool(); }));
    EXPECT_EQ(ann_.state(), State::InRoom);
    EXPECT_EQ(members(bob_), 1);

    // And again: one room, any number of shares.
    ann_.share(true, false);
    EXPECT_TRUE(wait_for([&] { return bob_.watching() == ann; }));
}

TEST_F(CallControllerTest, WhatIsWatchedIsChosen) {
    both_in();
    ann_.share(false, false);
    const QString ann = id_of(bob_, "Ann");
    ASSERT_TRUE(wait_for([&] { return bob_.watching() == ann; }));
    EXPECT_EQ(bob_.watched().value("name").toString(), "Ann") << "whose share, by name";
    EXPECT_EQ(bob_.watched().value("peerId").toString(), ann);
    EXPECT_EQ(g_screens_kept, 1) << "Bob's screen kept on while he watches";

    bob_.watch("");
    EXPECT_TRUE(wait_for([&] { return bob_.watching().isEmpty(); }));
    EXPECT_TRUE(bob_.watched().isEmpty());
    EXPECT_EQ(g_screens_kept, 0) << "and let go when he stops";
    EXPECT_TRUE(bob_view_.pictureSize().isEmpty());
    EXPECT_TRUE(member(bob_, "Ann").value("sharing").toBool()) << "still on offer";

    bob_.watch(ann);
    EXPECT_TRUE(wait_for(
        [&] { return bob_.watching() == ann && bob_view_.pictureSize() == QSize(320, 240); }));
    EXPECT_EQ(g_screens_kept, 1);
    bob_.leave();
    EXPECT_EQ(g_screens_kept, 0) << "nothing watched once out of the room";
}

TEST_F(CallControllerTest, EachSeesEveryCameraInAViewOfItsOwnAndItsOwn) {
    VideoView ann_preview;
    VideoView bob_preview;
    VideoView bob_at_ann;
    VideoView ann_at_bob;
    ann_.setPreview(&ann_preview);
    bob_.setPreview(&bob_preview);

    // Bob's on before he joins; Ann's once he is there.
    bob_.setCamera(true);
    both_in();
    ann_.setCameraView(id_of(ann_, "Bob"), &bob_at_ann);
    bob_.setCameraView(id_of(bob_, "Ann"), &ann_at_bob);
    ann_.setCamera(true);
    EXPECT_TRUE(wait_for([&] {
        return bob_at_ann.pictureSize() == QSize(160, 120) &&
               ann_at_bob.pictureSize() == QSize(160, 120) &&
               ann_preview.pictureSize() == QSize(160, 120) &&
               bob_preview.pictureSize() == QSize(160, 120);
    })) << "each camera seen at both ends";
    EXPECT_TRUE(wait_for([&] { return ann_.cameras() == 1 && bob_.cameras() == 1; }));
    EXPECT_TRUE(member(ann_, "Bob").value("camera").toBool());

    // A camera off is that camera gone, and nothing else.
    bob_.setCamera(false);
    EXPECT_TRUE(bob_preview.pictureSize().isEmpty());
    EXPECT_TRUE(
        wait_for([&] { return bob_at_ann.pictureSize().isEmpty() && ann_.cameras() == 0; }));
    EXPECT_EQ(ann_at_bob.pictureSize(), QSize(160, 120));
    EXPECT_EQ(members(ann_), 1);

    bob_.leave();
    EXPECT_TRUE(ann_at_bob.pictureSize().isEmpty());
    EXPECT_TRUE(ann_.camera()) << "kept on, as the user left it";
}

TEST_F(CallControllerTest, WithoutACameraItIsTurnedOffAndTheRoomGoesOn) {
    CallController::Environment blind = environment();
    blind.open_camera = [] { return std::unique_ptr<VideoSource>{}; };
    bob_.setEnvironment(blind);
    bob_.setName("Bob");
    bob_.setCamera(true);
    both_in();
    EXPECT_FALSE(bob_.camera());
    EXPECT_FALSE(bob_.notice().isEmpty());
}

TEST_F(CallControllerTest, EachSideReportsHowTheCallGoes) {
    const auto titles = [](const CallController& side) {
        QStringList titles;
        for (const QVariant& section : side.stats()) {
            titles.push_back(section.toMap().value("title").toString());
        }
        return titles;
    };
    int reports = 0;
    QObject::connect(&bob_, &CallController::statsChanged, [&] { ++reports; });
    bob_.setStatsEvery(1);
    testing::internal::CaptureStdout();
    both_in();
    ann_.share(false, true);
    ASSERT_TRUE(wait_for([&] {
        return titles(bob_).contains("Pictures") && titles(ann_).contains("Sent: screen") &&
               reports >= 2;
    })) << titles(bob_).join(", ").toStdString()
        << " / " << titles(ann_).join(", ").toStdString();
    const std::string printed = testing::internal::GetCapturedStdout();

    // The call first, with its CPU; a quality, and a line a part printed.
    EXPECT_EQ(titles(bob_).value(0), "Call");
    const QVariantMap call = bob_.stats().value(0).toMap();
    EXPECT_EQ(call.value("rows").toList().value(0).toMap().value("label").toString(), "CPU");
    EXPECT_FALSE(bob_.quality().isEmpty());
    EXPECT_NE(printed.find("[   1s] "), std::string::npos) << printed;
    EXPECT_NE(printed.find("voice "), std::string::npos) << printed;
    EXPECT_TRUE(ann_.statsEvery() == 0 && bob_.statsEvery() == 1);

    bob_.leave();
    EXPECT_TRUE(bob_.stats().isEmpty());
    EXPECT_TRUE(bob_.quality().isEmpty());
}

TEST_F(CallControllerTest, AShareThatRunsOutStopsAndTheRoomGoesOn) {
    CallController::Environment short_share = environment();
    short_share.open_share = [](const ScreenShareConfig&, std::stop_token) {
        return Result<ScreenShareSources>(
            ScreenShareSources{.pictures = std::make_unique<ShortPattern>()});
    };
    ann_.setEnvironment(short_share);
    make_room();
    ann_.share(true, false);
    ASSERT_TRUE(wait_for([&] { return ann_.sharing(); }));
    EXPECT_TRUE(wait_for([&] { return !ann_.sharing(); }));
    EXPECT_FALSE(ann_.notice().isEmpty()) << "the desktop stopped it, not the user";
    EXPECT_EQ(ann_.state(), State::InRoom);
}

// A room met at a server: the server is nobody's member. One that is none is
// said so, and no room is made.
TEST_F(CallControllerTest, ARoomIsMetAtTheServerNamedWhichIsNoMember) {
    ann_.setServer("not a server:");
    ann_.create();
    EXPECT_EQ(ann_.state(), State::Idle);
    EXPECT_FALSE(ann_.notice().isEmpty());

    librats::NodeConfig node;
    node.bind_address = "127.0.0.1";
    node.protocol = environment().call.protocol;
    librats::Node server(node);
    librats::Rendezvous::Config serving;
    serving.serve = true;
    server.add_subsystem(std::make_unique<librats::Rendezvous>(serving));
    ASSERT_TRUE(server.start());
    const QString at = QString("127.0.0.1:%1").arg(server.listen_port());
    ann_.setServer(at);
    bob_.setServer(at);
    both_in();
    ASSERT_TRUE(wait_for([&] { return server.peer_count() == 2; }));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(members(ann_), 1);
    EXPECT_EQ(members(bob_), 1);
    bob_.leave();
    ann_.leave();
    server.stop();
}

TEST_F(CallControllerTest, CallingOffTheDialogIsNoNews) {
    CallController::Environment declined = environment();
    declined.open_share = [](const ScreenShareConfig&, std::stop_token) {
        return Result<ScreenShareSources>(fail(Errc::Cancelled));
    };
    ann_.setEnvironment(declined);
    make_room();
    ann_.share(false, true);
    EXPECT_TRUE(ann_.choosing());
    ASSERT_TRUE(wait_for([&] { return !ann_.choosing(); }));
    EXPECT_FALSE(ann_.sharing());
    EXPECT_TRUE(ann_.notice().isEmpty());
}

TEST_F(CallControllerTest, EveryShareIsChosenInTheDialog) {
    // The identity kept, but not the desktop's last choice: that would stand
    // for the dialog from then on.
    QTemporaryDir data;
    ASSERT_TRUE(data.isValid());
    CallController::Environment kept = environment();
    kept.data_dir = data.path().toStdString();
    std::string asked_with = "unasked";
    kept.open_share = [&asked_with, open = kept.open_share](const ScreenShareConfig& config,
                                                            std::stop_token cancel) {
        asked_with = config.data_dir;
        return open(config, std::move(cancel));
    };
    ann_.setEnvironment(kept);
    make_room();
    ann_.share(false, false);
    ASSERT_TRUE(wait_for([&] { return ann_.sharing(); })) << ann_.notice().toStdString();
    EXPECT_EQ(asked_with, "") << "no choice kept to restore";
}

TEST_F(CallControllerTest, AShareTheDesktopCannotMakeSaysWhy) {
    CallController::Environment unavailable = environment();
    unavailable.open_share = [](const ScreenShareConfig&, std::stop_token) {
        return Result<ScreenShareSources>(fail(Errc::Unavailable));
    };
    ann_.setEnvironment(unavailable);
    make_room();
    ann_.share(false, true);
    ASSERT_TRUE(wait_for([&] { return !ann_.choosing(); }));
    EXPECT_TRUE(ann_.notice().contains("unavailable")) << ann_.notice().toStdString();
    EXPECT_EQ(ann_.state(), State::InRoom);
}

TEST_F(CallControllerTest, LeavingOrStoppingWhileTheDialogIsOpenCallsItOff) {
    std::atomic<int> called_off{0};
    CallController::Environment waiting = environment();
    waiting.open_share = [&](const ScreenShareConfig&, std::stop_token cancel) {
        while (!cancel.stop_requested()) std::this_thread::sleep_for(1ms);  // the user ponders
        ++called_off;
        return Result<ScreenShareSources>(fail(Errc::Cancelled));
    };
    ann_.setEnvironment(waiting);
    make_room();
    ann_.share(false, true);
    ann_.stopSharing();
    ASSERT_TRUE(wait_for([&] { return !ann_.choosing(); }));
    EXPECT_EQ(called_off, 1);
    EXPECT_TRUE(ann_.notice().isEmpty());

    ann_.share(false, true);
    ann_.leave();
    EXPECT_EQ(called_off, 2);
    EXPECT_EQ(ann_.state(), State::Idle);
    EXPECT_FALSE(ann_.choosing());
    QCoreApplication::processEvents();  // the dialog's answer, dropped
    EXPECT_EQ(ann_.state(), State::Idle);
    EXPECT_TRUE(ann_.notice().isEmpty());
}

TEST_F(CallControllerTest, ALinkThatIsNoneIsRefused) {
    bob_.join("http://example.org");
    EXPECT_EQ(bob_.state(), State::Idle);
    EXPECT_FALSE(bob_.notice().isEmpty());
}

}  // namespace

int main(int argc, char** argv) {
    // Nothing is drawn: no display needed.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
