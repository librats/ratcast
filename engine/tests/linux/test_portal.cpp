// The portal client against a fake portal on a private D-Bus: what it asks
// for, what it makes of the answers, and that it leaves no session open
// whatever the outcome; the screen kept on through it, and let go. Skipped
// where dbus-daemon is missing.

#include "linux/portal.h"

#include <gio/gio.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "fake_portal.h"
#include "ratcast/engine/screen_awake.h"

using namespace ratcast;
using test::FakePortal;

namespace {

class PortalTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        gchar* daemon = g_find_program_in_path("dbus-daemon");
        if (!daemon) return;
        g_free(daemon);
        if (const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS")) saved_address_ = address;
        bus_ = g_test_dbus_new(G_TEST_DBUS_NONE);
        g_test_dbus_up(bus_);  // and points DBUS_SESSION_BUS_ADDRESS at it
    }

    static void TearDownTestSuite() {
        if (!bus_) return;
        g_test_dbus_down(bus_);
        g_object_unref(bus_);
        bus_ = nullptr;
        if (saved_address_) setenv("DBUS_SESSION_BUS_ADDRESS", saved_address_->c_str(), 1);
    }

    void SetUp() override {
        if (!bus_) GTEST_SKIP() << "no dbus-daemon here";
    }

    std::unique_ptr<FakePortal> portal(FakePortal::Config config = {}) {
        return std::make_unique<FakePortal>(g_test_dbus_get_bus_address(bus_), std::move(config));
    }

    inline static GTestDBus* bus_ = nullptr;
    inline static std::optional<std::string> saved_address_;
};

}  // namespace

TEST_F(PortalTest, GrantsACastAndAsksForWhatWasWanted) {
    auto fake = portal();
    portal::ScreenCastRequest request;
    request.restore_token = "restore-0";
    Result<std::unique_ptr<portal::ScreenCast>> cast = portal::start_screen_cast(request);
    ASSERT_TRUE(cast) << to_string(cast.error());

    EXPECT_EQ((*cast)->node(), 42u);
    EXPECT_EQ((*cast)->restore_token(), "restore-1");
    const FakePortal::Seen seen = fake->seen();
    EXPECT_EQ(seen.types, 1u | 2u);
    EXPECT_EQ(seen.multiple, false);
    EXPECT_EQ(seen.cursor_mode, 2u);   // embedded
    EXPECT_EQ(seen.persist_mode, 2u);  // until revoked
    EXPECT_EQ(seen.restore_token, "restore-0");

    // The descriptor is the one the portal handed out.
    ASSERT_GE((*cast)->pipewire_fd(), 0);
    ASSERT_EQ(write(fake->peer_fd(), "x", 1), 1);
    char byte = 0;
    ASSERT_EQ(read((*cast)->pipewire_fd(), &byte, 1), 1);
    EXPECT_EQ(byte, 'x');

    EXPECT_EQ(fake->seen().sessions_closed, 0);
    cast->reset();
    EXPECT_EQ(fake->seen().sessions_closed, 1);
}

TEST_F(PortalTest, AsksOnlyForWhatThePortalOffers) {
    FakePortal::Config config;
    config.version = 3;       // no persistence before version 4
    config.source_types = 1;  // no windows
    config.cursor_modes = 1;  // the pointer cannot be drawn in
    auto fake = portal(config);

    portal::ScreenCastRequest request;
    request.restore_token = "restore-0";
    ASSERT_TRUE(portal::start_screen_cast(request));
    const FakePortal::Seen seen = fake->seen();
    EXPECT_EQ(seen.types, 1u);
    EXPECT_EQ(seen.cursor_mode, std::nullopt);
    EXPECT_EQ(seen.persist_mode, std::nullopt);
    EXPECT_EQ(seen.restore_token, std::nullopt);
}

TEST_F(PortalTest, HidesThePointerWhenAskedTo) {
    auto fake = portal();
    portal::ScreenCastRequest request;
    request.cursor = false;
    ASSERT_TRUE(portal::start_screen_cast(request));
    EXPECT_EQ(fake->seen().cursor_mode, 1u);
}

TEST_F(PortalTest, WindowsAloneFromAPortalWithoutThemIsUnavailable) {
    FakePortal::Config config;
    config.source_types = 1;
    auto fake = portal(config);
    portal::ScreenCastRequest request;
    request.monitors = false;
    Result<std::unique_ptr<portal::ScreenCast>> cast = portal::start_screen_cast(request);
    ASSERT_FALSE(cast);
    EXPECT_EQ(cast.error(), Errc::Unavailable);
}

TEST_F(PortalTest, TheUserDecliningIsCancelled) {
    FakePortal::Config config;
    config.start_response = 1;
    auto fake = portal(config);
    Result<std::unique_ptr<portal::ScreenCast>> cast = portal::start_screen_cast({});
    ASSERT_FALSE(cast);
    EXPECT_EQ(cast.error(), Errc::Cancelled);
    EXPECT_EQ(fake->seen().sessions_closed, 1);
}

TEST_F(PortalTest, APortalFailureIsUnavailable) {
    FakePortal::Config config;
    config.start_response = 2;
    auto fake = portal(config);
    Result<std::unique_ptr<portal::ScreenCast>> cast = portal::start_screen_cast({});
    ASSERT_FALSE(cast);
    EXPECT_EQ(cast.error(), Errc::Unavailable);
    EXPECT_EQ(fake->seen().sessions_closed, 1);
}

TEST_F(PortalTest, CallingItOffClosesTheDialog) {
    FakePortal::Config config;
    config.answer_start = false;
    auto fake = portal(config);

    std::stop_source stop;
    auto waiting = std::async(std::launch::async,
                              [&] { return portal::start_screen_cast({}, stop.get_token()); });
    ASSERT_TRUE(fake->wait_until_started());
    stop.request_stop();

    Result<std::unique_ptr<portal::ScreenCast>> cast = waiting.get();
    ASSERT_FALSE(cast);
    EXPECT_EQ(cast.error(), Errc::Cancelled);
    EXPECT_EQ(fake->seen().requests_closed, 1);
    EXPECT_EQ(fake->seen().sessions_closed, 1);
}

TEST_F(PortalTest, NoPortalIsUnavailable) {
    Result<std::unique_ptr<portal::ScreenCast>> cast = portal::start_screen_cast({});
    ASSERT_FALSE(cast);
    EXPECT_EQ(cast.error(), Errc::Unavailable);
}

TEST_F(PortalTest, AskingForNothingIsInvalid) {
    portal::ScreenCastRequest request;
    request.monitors = false;
    request.windows = false;
    EXPECT_EQ(portal::start_screen_cast(request).error(), Errc::InvalidArgument);
}

TEST_F(PortalTest, KeepsTheScreenOnUntilLetGo) {
    auto fake = portal();
    std::unique_ptr<ScreenAwake> awake = keep_screen_awake("Watching a share");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!awake->held() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(awake->held());
    const FakePortal::Seen seen = fake->seen();
    EXPECT_EQ(seen.inhibitions, 1);
    EXPECT_EQ(seen.inhibit_flags, 8u) << "idleness, and nothing else";
    EXPECT_EQ(seen.inhibit_reason, "Watching a share");
    EXPECT_EQ(seen.requests_closed, 0);

    awake.reset();
    EXPECT_TRUE(fake->wait_until([](const FakePortal::Seen& s) { return s.requests_closed == 1; }))
        << "let go: the inhibition's request closed";
}

TEST_F(PortalTest, APortalThatCannotInhibitLeavesTheScreenAlone) {
    auto fake = portal({.inhibits = false});
    Result<std::unique_ptr<portal::Inhibition>> inhibition = portal::inhibit_idle("Watching");
    ASSERT_FALSE(inhibition);
    EXPECT_EQ(inhibition.error(), Errc::Unavailable);
    std::unique_ptr<ScreenAwake> awake = keep_screen_awake("Watching");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(awake->held());
}

TEST_F(PortalTest, NoPortalNoInhibition) {
    EXPECT_EQ(portal::inhibit_idle("Watching").error(), Errc::Unavailable);
    // Asked and let go at once: it never waits.
    keep_screen_awake("Watching").reset();
}
