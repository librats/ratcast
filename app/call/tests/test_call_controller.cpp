// CallController between two of its kind on loopback: one shares (the test
// pattern and a tone standing in for the desktop's screen and sound), the
// other joins by an invite and watches, each going through its states; both
// talking, either side's camera seen by the other and by itself, each
// listing the other by the name it gives, each reporting how the call goes;
// and the ways a share or a join ends without the user.

#include "call_controller.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QGuiApplication>

#include <atomic>
#include <chrono>
#include <thread>

#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/tone_source.h"
#include "ratcast/engine/virtual_output.h"

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
        sharer_.setEnvironment(environment());
        viewer_.setEnvironment(environment());
        viewer_.setVideo(&view_);
    }

    /// The sharer sharing, and its invite on loopback.
    QString share() {
        sharer_.share(false, true);
        EXPECT_TRUE(wait_for([&] { return sharer_.state() == State::Sharing; }))
            << sharer_.notice().toStdString();
        EXPECT_EQ(sharer_.invites().size(), 1) << "one address: the one it listens on";
        return sharer_.invites().value(0);
    }

    VideoView view_;
    CallController sharer_;
    CallController viewer_;
};

TEST_F(CallControllerTest, OneSharesTheOtherJoinsAndWatches) {
    const QString invite = share();
    EXPECT_TRUE(invite.startsWith("rats://127.0.0.1:"));
    EXPECT_EQ(sharer_.viewers(), 0);

    viewer_.join(invite);
    EXPECT_EQ(viewer_.state(), State::Joining);
    ASSERT_TRUE(wait_for([&] { return viewer_.state() == State::Watching; }));
    EXPECT_TRUE(wait_for([&] { return view_.pictureSize() == QSize(320, 240); }))
        << "the pictures reach the view";
    EXPECT_TRUE(wait_for([&] { return sharer_.viewers() == 1; }));
    EXPECT_TRUE(viewer_.notice().isEmpty());

    viewer_.leave();
    EXPECT_EQ(viewer_.state(), State::Idle);
    EXPECT_TRUE(view_.pictureSize().isEmpty()) << "the view cleared";
    EXPECT_TRUE(wait_for([&] { return sharer_.viewers() == 0; }));
    EXPECT_EQ(sharer_.state(), State::Sharing) << "still sharing, for the next to join";
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
    QObject::connect(&viewer_, &CallController::statsChanged, [&] { ++reports; });
    viewer_.setStatsEvery(1);
    testing::internal::CaptureStdout();
    viewer_.join(share());
    ASSERT_TRUE(wait_for([&] {
        return titles(viewer_).contains("Pictures") && titles(sharer_).contains("Sent: screen") &&
               reports >= 2;
    })) << titles(viewer_).join(", ").toStdString()
        << " / " << titles(sharer_).join(", ").toStdString();
    const std::string printed = testing::internal::GetCapturedStdout();

    // The call first, with its CPU; a quality, and a line a part printed.
    EXPECT_EQ(titles(viewer_).value(0), "Call");
    const QVariantMap call = viewer_.stats().value(0).toMap();
    EXPECT_EQ(call.value("rows").toList().value(0).toMap().value("label").toString(), "CPU");
    EXPECT_FALSE(viewer_.quality().isEmpty());
    EXPECT_NE(printed.find("[   1s] "), std::string::npos) << printed;
    EXPECT_NE(printed.find("pictures"), std::string::npos) << printed;
    EXPECT_TRUE(sharer_.statsEvery() == 0 && viewer_.statsEvery() == 1);

    viewer_.leave();
    EXPECT_TRUE(viewer_.stats().isEmpty());
    EXPECT_TRUE(viewer_.quality().isEmpty());
}

TEST_F(CallControllerTest, EachListsTheOtherByTheNameItGives) {
    // The member a controller lists, once there is exactly one.
    const auto member = [](const CallController& controller) {
        const QVariantList members = controller.members();
        return members.size() == 1 ? members.front().toMap() : QVariantMap{};
    };
    sharer_.setName("Ann");
    viewer_.setName("Bob");
    viewer_.join(share());
    ASSERT_TRUE(wait_for([&] {
        return member(sharer_).value("name") == "Bob" && member(viewer_).value("name") == "Ann";
    }));
    EXPECT_FALSE(member(sharer_).value("muted").toBool());
    EXPECT_EQ(member(sharer_).value("peer").toString().size(), 8) << "a short id beside it";

    // Muted, and renamed: told at once.
    viewer_.setMuted(true);
    EXPECT_TRUE(wait_for([&] { return member(sharer_).value("muted").toBool(); }));
    viewer_.setName(QString("Robert\n\tthe   ") + QString(200, QChar(0x044F)));  // 'я'
    ASSERT_TRUE(wait_for([&] { return member(sharer_).value("name") != "Bob"; }));
    const QString told = member(sharer_).value("name").toString();
    EXPECT_TRUE(told.startsWith("Robert the яя")) << told.toStdString();
    EXPECT_LE(told.toUtf8().size(), qsizetype(Presence::kMaxNameSize));

    viewer_.leave();
    EXPECT_TRUE(viewer_.members().isEmpty());
    EXPECT_TRUE(wait_for([&] { return sharer_.members().isEmpty(); }));
}

TEST_F(CallControllerTest, BothTalkAndAMutedOneIsNotHeard) {
    viewer_.join(share());
    ASSERT_TRUE(wait_for([&] { return viewer_.state() == State::Watching; }));
    EXPECT_TRUE(wait_for([&] { return sharer_.hearing() && viewer_.hearing(); }))
        << "the sharer hears the viewer's voice, the viewer the sharer's and the screen's";

    viewer_.setMuted(true);
    EXPECT_FALSE(viewer_.speaking());
    EXPECT_TRUE(wait_for([&] { return !sharer_.hearing(); }));
    viewer_.setMuted(false);
    EXPECT_TRUE(wait_for([&] { return sharer_.hearing(); }));

    viewer_.leave();
    EXPECT_FALSE(viewer_.hearing());
}

TEST_F(CallControllerTest, WithoutAMicrophoneTheCallGoesOn) {
    CallController::Environment deaf = environment();
    deaf.open_microphone = [] { return std::unique_ptr<AudioSource>{}; };
    viewer_.setEnvironment(deaf);
    viewer_.join(share());
    ASSERT_TRUE(wait_for([&] { return viewer_.state() == State::Watching; }));
    EXPECT_FALSE(viewer_.notice().isEmpty()) << "still said once the picture plays";
    EXPECT_TRUE(wait_for([&] { return !sharer_.hearing() && viewer_.hearing(); }));
}

TEST_F(CallControllerTest, EachSeesTheOthersCameraAndItsOwn) {
    VideoView sharer_camera;
    VideoView sharer_preview;
    VideoView viewer_camera;
    VideoView viewer_preview;
    sharer_.setCameraView(&sharer_camera);
    sharer_.setPreview(&sharer_preview);
    viewer_.setCameraView(&viewer_camera);
    viewer_.setPreview(&viewer_preview);

    // The viewer's on before it joins; the sharer's while it shares.
    viewer_.setCamera(true);
    viewer_.join(share());
    ASSERT_TRUE(wait_for([&] { return viewer_.state() == State::Watching; }));
    sharer_.setCamera(true);
    ASSERT_TRUE(wait_for([&] { return sharer_.cameraShown() && viewer_.cameraShown(); }));
    EXPECT_TRUE(wait_for([&] {
        return sharer_camera.pictureSize() == QSize(160, 120) &&
               viewer_camera.pictureSize() == QSize(160, 120) &&
               sharer_preview.pictureSize() == QSize(160, 120) &&
               viewer_preview.pictureSize() == QSize(160, 120);
    })) << "each camera seen at both ends";
    // Watching is the screen's player started: its first picture may come after the cameras'.
    EXPECT_TRUE(wait_for([&] { return view_.pictureSize() == QSize(320, 240); }))
        << "the screen apart";

    // A camera off is not the screen gone.
    viewer_.setCamera(false);
    EXPECT_TRUE(viewer_preview.pictureSize().isEmpty());
    EXPECT_TRUE(wait_for([&] { return !sharer_.cameraShown(); }));
    EXPECT_TRUE(sharer_camera.pictureSize().isEmpty());
    EXPECT_EQ(viewer_.state(), State::Watching);
    EXPECT_TRUE(viewer_.cameraShown());

    viewer_.leave();
    EXPECT_FALSE(viewer_.cameraShown());
    EXPECT_TRUE(viewer_camera.pictureSize().isEmpty());
    EXPECT_TRUE(sharer_.camera()) << "kept on, as the user left it";
}

TEST_F(CallControllerTest, WithoutACameraItIsTurnedOffAndTheCallGoesOn) {
    CallController::Environment blind = environment();
    blind.open_camera = [] { return std::unique_ptr<VideoSource>{}; };
    viewer_.setEnvironment(blind);
    viewer_.setCamera(true);
    viewer_.join(share());
    EXPECT_FALSE(viewer_.camera());
    ASSERT_TRUE(wait_for([&] { return viewer_.state() == State::Watching; }));
    EXPECT_FALSE(viewer_.notice().isEmpty()) << "still said once the picture plays";
}

TEST_F(CallControllerTest, TheSharerLeavingIsNews) {
    viewer_.join(share());
    ASSERT_TRUE(wait_for([&] { return viewer_.state() == State::Watching; }));
    sharer_.leave();
    EXPECT_TRUE(sharer_.invites().isEmpty());
    // The stream ends first, then the peer goes: waiting, and told why.
    EXPECT_TRUE(
        wait_for([&] { return viewer_.state() == State::Joining && !viewer_.notice().isEmpty(); }));
}

TEST_F(CallControllerTest, AShareThatRunsOutEnds) {
    CallController::Environment short_share = environment();
    short_share.open_share = [](const ScreenShareConfig&, std::stop_token) {
        return Result<ScreenShareSources>(
            ScreenShareSources{.pictures = std::make_unique<ShortPattern>()});
    };
    sharer_.setEnvironment(short_share);
    sharer_.share(true, false);
    ASSERT_TRUE(wait_for([&] { return sharer_.state() == State::Sharing; }));
    EXPECT_TRUE(wait_for([&] { return sharer_.state() == State::Idle; }));
    EXPECT_FALSE(sharer_.notice().isEmpty()) << "the desktop stopped it, not the user";
    EXPECT_TRUE(sharer_.invites().isEmpty());
}

TEST_F(CallControllerTest, CallingOffTheDialogIsNoNews) {
    CallController::Environment declined = environment();
    declined.open_share = [](const ScreenShareConfig&, std::stop_token) {
        return Result<ScreenShareSources>(fail(Errc::Cancelled));
    };
    sharer_.setEnvironment(declined);
    sharer_.share(false, true);
    EXPECT_EQ(sharer_.state(), State::Choosing);
    ASSERT_TRUE(wait_for([&] { return sharer_.state() == State::Idle; }));
    EXPECT_TRUE(sharer_.notice().isEmpty());
}

TEST_F(CallControllerTest, AShareTheDesktopCannotMakeSaysWhy) {
    CallController::Environment unavailable = environment();
    unavailable.open_share = [](const ScreenShareConfig&, std::stop_token) {
        return Result<ScreenShareSources>(fail(Errc::Unavailable));
    };
    sharer_.setEnvironment(unavailable);
    sharer_.share(false, true);
    ASSERT_TRUE(wait_for([&] { return sharer_.state() == State::Idle; }));
    EXPECT_TRUE(sharer_.notice().contains("unavailable")) << sharer_.notice().toStdString();
}

TEST_F(CallControllerTest, LeavingWhileTheDialogIsOpenCallsItOff) {
    std::atomic<bool> called_off{false};
    CallController::Environment waiting = environment();
    waiting.open_share = [&](const ScreenShareConfig&, std::stop_token cancel) {
        while (!cancel.stop_requested()) std::this_thread::sleep_for(1ms);  // the user ponders
        called_off = true;
        return Result<ScreenShareSources>(fail(Errc::Cancelled));
    };
    sharer_.setEnvironment(waiting);
    sharer_.share(false, true);
    sharer_.leave();
    EXPECT_TRUE(called_off);
    EXPECT_EQ(sharer_.state(), State::Idle);
    QCoreApplication::processEvents();  // the dialog's answer, dropped
    EXPECT_EQ(sharer_.state(), State::Idle);
    EXPECT_TRUE(sharer_.notice().isEmpty());
}

TEST_F(CallControllerTest, ALinkThatIsNoneIsRefused) {
    viewer_.join("http://example.org");
    EXPECT_EQ(viewer_.state(), State::Idle);
    EXPECT_FALSE(viewer_.notice().isEmpty());
}

}  // namespace

int main(int argc, char** argv) {
    // Nothing is drawn: no display needed.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
