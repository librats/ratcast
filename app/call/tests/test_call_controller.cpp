// CallController between two of its kind on loopback: one shares (the test
// pattern and a tone standing in for the desktop's screen and sound), the
// other joins by an invite and watches, each going through its states; and
// the ways a share or a join ends without the user.

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
/// the screen and a tone for its sound, a speaker that plays to nobody.
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
