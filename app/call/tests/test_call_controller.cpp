// CallController in rooms on loopback: one makes a room and others join it by
// its invites or by each other's, each going through its states; everyone
// talking and seen speaking, muted, deafened, once the microphone is
// allowed, the system told of the call meanwhile; anyone sharing (the test
// pattern and a tone standing in for the desktop's screen and sound) and
// stopping while the room goes on, the others watching it or not; cameras,
// each member's in a view of its own; the devices chosen, kept and used at
// once; each listing the others by the names they give; each reporting how
// the call goes; and the ways a share or a join ends without the user.

#include "call_controller.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "ratcast/client/rats_url.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/tone_source.h"
#include "ratcast/engine/virtual_output.h"
#include "udp_path.h"

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

/// Calls the system is told of, by every side, and how often told again.
std::atomic<int> g_calls_told{0};
std::atomic<int> g_calls_updated{0};

class Told final : public InCall {
public:
    Told() { ++g_calls_told; }
    ~Told() override { --g_calls_told; }
    void update() override { ++g_calls_updated; }
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
    environment.call.built_in_servers = {};
    environment.call.dht = false;  // a room met by its secret: at a test's server, if any
    environment.open_share = [](const ScreenShareConfig& config,
                                std::stop_token) -> Result<ScreenShareSources> {
        ScreenShareSources sources{.pictures = std::make_unique<PatternSource>(
                                       PatternConfig{PixelFormat::NV12, 320, 240, 30, 1})};
        if (config.sound) {
            sources.sound = std::make_unique<ToneSource>(ToneConfig{.format = {48'000, 2}});
        }
        return sources;
    };
    environment.open_speaker = [](const std::string&) {
        return std::make_unique<VirtualOutput>(VirtualOutputConfig{});
    };
    environment.open_microphone = [](const std::string&) {
        return std::make_unique<ToneSource>(ToneConfig{.frequency = 300, .level = 0.3f});
    };
    environment.open_camera = [](const std::string&) {
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
    /// Ann's new room, and its link by the address it listens on: no server
    /// to meet at by its secret.
    QString make_room() {
        ann_.create();
        EXPECT_EQ(ann_.state(), State::InRoom) << ann_.notice().toStdString();
        EXPECT_EQ(ann_.invites().size(), 2) << "its secret, and the address it listens on";
        return address(ann_);
    }

    /// `side`'s room's link by the address it listens on, on loopback.
    static QString address(const CallController& side) {
        for (const QString& invite : side.invites())
            if (invite.startsWith("https://ratcast.app/r/#127.0.0.1:")) return invite;
        return {};
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

TEST(DataFolderTest, WhatRoomsNodesKeptIsTakenAwayAndThePersonsStays) {
    QTemporaryDir data;
    ASSERT_TRUE(data.isValid());
    const std::filesystem::path folder(data.path().toStdString());
    std::filesystem::create_directories(folder / "guest");
    std::filesystem::create_directories(folder / "home");
    const std::vector<std::filesystem::path> rooms = {"identity.key", "dht_routing.json",
                                                      "dht_routing_v6.json", "guest/identity.key",
                                                      "guest/dht_routing.json"};
    const std::vector<std::filesystem::path> kept = {"person.key",        "people.json",
                                                     "servers",           "ratcast.log",
                                                     "home/identity.key", "home/dht_routing.json"};
    for (const auto& file : rooms) std::ofstream(folder / file) << "x";
    for (const auto& file : kept) std::ofstream(folder / file) << "x";

    forget_rooms_nodes(data.path());
    for (const auto& file : rooms) EXPECT_FALSE(std::filesystem::exists(folder / file)) << file;
    EXPECT_FALSE(std::filesystem::exists(folder / "guest"));
    for (const auto& file : kept) EXPECT_TRUE(std::filesystem::exists(folder / file)) << file;
    forget_rooms_nodes(data.path());  // nothing more to take: no harm
}

TEST_F(CallControllerTest, OneMakesARoomAndAnotherJoins) {
    const QString invite = make_room();
    EXPECT_TRUE(invite.startsWith("https://ratcast.app/r/#127.0.0.1:")) << invite.toStdString();
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

TEST_F(CallControllerTest, WhatIsSaidIsListedByEachAndReadByOneWhoComesLater) {
    both_in();
    const auto at = [](CallController& side, int row, ChatModel::Role role) {
        ChatModel& chat = *side.chat();
        return chat.data(chat.index(row), role);
    };
    EXPECT_TRUE(ann_.say("  hi <b>Bob</b>\r\nsee https://example.com.\x07 "));
    ASSERT_EQ(ann_.chat()->rowCount(), 1);
    EXPECT_TRUE(at(ann_, 0, ChatModel::MineRole).toBool());
    EXPECT_EQ(ann_.chat()->unread(), 0) << "one's own is read";

    ASSERT_TRUE(wait_for([&] { return bob_.chat()->rowCount() == 1; }));
    EXPECT_EQ(at(bob_, 0, ChatModel::TextRole), "hi <b>Bob</b>\nsee https://example.com.");
    EXPECT_EQ(at(bob_, 0, ChatModel::NameRole), "Ann");
    EXPECT_EQ(at(bob_, 0, ChatModel::AuthorRole), id_of(bob_, "Ann"));
    EXPECT_FALSE(at(bob_, 0, ChatModel::MineRole).toBool());
    EXPECT_EQ(bob_.chat()->unread(), 1);
    bob_.chat()->setReading(true);
    EXPECT_EQ(bob_.chat()->unread(), 0);

    EXPECT_TRUE(bob_.say("hello"));
    ASSERT_TRUE(wait_for([&] { return ann_.chat()->rowCount() == 2; }));
    EXPECT_EQ(at(ann_, 1, ChatModel::NameRole), "Bob");
    EXPECT_FALSE(at(ann_, 1, ChatModel::ContinuedRole).toBool()) << "another's";

    // Carl comes later, and reads it all.
    carl_.join(bob_.invites().value(0));
    ASSERT_TRUE(wait_for([&] { return carl_.chat()->rowCount() == 2; }));
    EXPECT_EQ(at(carl_, 0, ChatModel::NameRole), "Ann");
    EXPECT_EQ(at(carl_, 1, ChatModel::TextRole), "hello");
    EXPECT_EQ(carl_.chat()->unread(), 2);

    EXPECT_FALSE(ann_.say(" \n\t "));
    EXPECT_TRUE(ann_.notice().isEmpty()) << "nothing said is no news";
    EXPECT_FALSE(ann_.say(QString(qsizetype(ChatMessage::kMaxTextSize) / 2 + 1, QChar(0x044F))));
    EXPECT_FALSE(ann_.notice().isEmpty()) << "too long for its bytes";
    EXPECT_EQ(ann_.chat()->rowCount(), 2);

    // A new room starts with nothing said.
    ann_.leave();
    ann_.create();
    EXPECT_EQ(ann_.chat()->rowCount(), 0);
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

TEST_F(CallControllerTest, EachMemberIsHeardAsLoudAsSetAndItIsKept) {
    // Ann's speaker, listened to: the loudest it played over a while.
    auto loudest = std::make_shared<std::atomic<float>>(0.0f);
    CallController::Environment ann = environment();
    ann.open_speaker = [loudest](const std::string&) {
        return std::make_unique<VirtualOutput>(
            VirtualOutputConfig{}, [loudest](std::span<const float> samples, Timestamp) {
                float block = 0.0f;
                for (const float sample : samples) block = std::max(block, std::abs(sample));
                float most = loudest->load();
                while (block > most && !loudest->compare_exchange_weak(most, block)) {}
            });
    };
    QTemporaryDir kept;
    ASSERT_TRUE(kept.isValid());
    const QString settings_file = kept.filePath("ratcast.conf");
    ann.settings = [settings_file] {
        return std::make_unique<QSettings>(settings_file, QSettings::IniFormat);
    };
    ann_.setEnvironment(ann);
    ann_.setName("Ann");
    const auto played = [&] {
        wait_for([] { return false; }, 100ms);  // what was pulled before played out
        *loudest = 0.0f;
        wait_for([] { return false; }, 300ms);
        return loudest->load();
    };

    both_in();
    const QString bob = id_of(ann_, "Bob");
    ASSERT_TRUE(wait_for([&] { return member(ann_, "Bob").value("speaking").toBool(); }));
    const float as_it_comes = played();
    EXPECT_GT(as_it_comes, 0.1f);

    ann_.setVolume(bob, 0.5);
    EXPECT_EQ(member(ann_, "Bob").value("volume").toDouble(), 0.5);
    const float half = played();
    EXPECT_GT(half, as_it_comes * 0.3f);
    EXPECT_LT(half, as_it_comes * 0.7f);

    ann_.setMutedHere(bob, true);
    EXPECT_TRUE(member(ann_, "Bob").value("mutedHere").toBool());
    EXPECT_EQ(played(), 0.0f);
    EXPECT_TRUE(member(ann_, "Bob").value("speaking").toBool()) << "still seen speaking";
    EXPECT_EQ(member(ann_, "Bob").value("volume").toDouble(), 0.5) << "the volume kept";
    EXPECT_FALSE(member(bob_, "Ann").value("muted").toBool()) << "the others know nothing";

    ann_.setVolume(bob, 7.0);
    EXPECT_EQ(member(ann_, "Bob").value("volume").toDouble(), CallController::kMaxVolume);

    // Kept for another run: Carl, on Ann's settings, hears Bob as Ann set
    // from when he joins Bob, who stays in the room Ann left.
    ann_.leave();
    CallController::Environment carl = environment();
    carl.settings = ann.settings;
    carl_.setEnvironment(carl);
    carl_.setName("Carl");
    carl_.join(bob_.invites().value(0));
    ASSERT_TRUE(wait_for([&] { return !member(carl_, "Bob").isEmpty(); }));
    EXPECT_EQ(member(carl_, "Bob").value("volume").toDouble(), CallController::kMaxVolume);
    EXPECT_TRUE(member(carl_, "Bob").value("mutedHere").toBool());

    // As it comes again: nothing kept.
    carl_.setVolume(bob, 1.0);
    carl_.setMutedHere(bob, false);
    EXPECT_TRUE(QSettings(settings_file, QSettings::IniFormat).childGroups().isEmpty());
}

TEST_F(CallControllerTest, AMemberCutOffIsListedByNameReconnectingUntilBack) {
    CallController::Environment quick = environment();
    quick.call.lost_after = 3s;
    for (CallController* side : {&ann_, &bob_}) side->setEnvironment(quick);
    ann_.setName("Ann");
    bob_.setName("Bob");
    const std::optional<RatsLink> invite = parse_rats_link(make_room().toStdString());
    ASSERT_TRUE(invite);
    test::UdpPath path(std::get<librats::HostEndpoint>(invite->url).port);
    bob_.join(QString("rats://127.0.0.1:%1").arg(path.port()));
    ASSERT_TRUE(wait_for([&] { return !member(ann_, "Bob").isEmpty(); }));
    EXPECT_FALSE(member(ann_, "Bob").value("reconnecting").toBool());

    // Cut off: Ann still lists him by name, reconnecting — quiet, then lost and
    // dialed again where he listens, which the dead path does not stand in.
    path.dead = true;
    EXPECT_TRUE(wait_for([&] { return member(ann_, "Bob").value("reconnecting").toBool(); }, 5s));
    EXPECT_TRUE(wait_for([&] {
        const QVariantMap bob = member(ann_, "Bob");
        return !bob.isEmpty() && !bob.value("reconnecting").toBool() && members(ann_) == 1;
    })) << "back";
}

TEST_F(CallControllerTest, WithoutAMicrophoneTheRoomGoesOn) {
    CallController::Environment mute = environment();
    mute.open_microphone = [](const std::string&) { return std::unique_ptr<AudioSource>{}; };
    bob_.setEnvironment(mute);
    bob_.setName("Bob");
    both_in();
    EXPECT_FALSE(bob_.notice().isEmpty());
    EXPECT_TRUE(wait_for([&] { return member(bob_, "Ann").value("speaking").toBool(); }));
    EXPECT_FALSE(member(ann_, "Bob").value("speaking").toBool());
}

TEST_F(CallControllerTest, AMicrophoneNotAllowedIsNotHeardAndTheRoomGoesOn) {
    CallController::Environment refused = environment();
    refused.ask_microphone = [](std::function<void(bool)> allowed) { allowed(false); };
    bob_.setEnvironment(refused);
    bob_.setName("Bob");
    both_in();
    EXPECT_FALSE(bob_.notice().isEmpty());
    EXPECT_TRUE(wait_for([&] { return member(bob_, "Ann").value("speaking").toBool(); }));
    EXPECT_FALSE(member(ann_, "Bob").value("speaking").toBool());
}

TEST_F(CallControllerTest, TheMicrophoneIsHeardOnceAllowedTheSystemToldOfTheCall) {
    std::function<void(bool)> answer;
    CallController::Environment asking = environment();
    asking.ask_microphone = [&](std::function<void(bool)> allowed) { answer = std::move(allowed); };
    asking.enter_call = [] { return std::make_unique<Told>(); };
    bob_.setEnvironment(asking);
    bob_.setName("Bob");
    g_calls_updated = 0;
    both_in();
    EXPECT_EQ(g_calls_told, 1);
    ASSERT_TRUE(answer) << "asked as the room started";
    EXPECT_TRUE(wait_for([&] { return member(bob_, "Ann").value("speaking").toBool(); }));
    EXPECT_FALSE(member(ann_, "Bob").value("speaking").toBool()) << "not before it is allowed";

    std::exchange(answer, {})(true);
    EXPECT_TRUE(wait_for([&] { return member(ann_, "Bob").value("speaking").toBool(); }));
    EXPECT_TRUE(bob_.notice().isEmpty()) << bob_.notice().toStdString();
    EXPECT_EQ(g_calls_updated, 1) << "told it may hear the microphone in the background";

    bob_.leave();
    EXPECT_EQ(g_calls_told, 0);
    // An answer that comes after the room it was asked in is over is dropped.
    bob_.join(address(ann_));
    ASSERT_TRUE(answer);
    bob_.leave();
    answer(false);
    EXPECT_TRUE(bob_.notice().isEmpty()) << bob_.notice().toStdString();
}

TEST_F(CallControllerTest, TheCameraIsSentOnceAllowedAskedAfterTheMicrophone) {
    std::function<void(bool)> microphone;
    std::function<void(bool)> camera;
    auto opened = std::make_shared<std::atomic<int>>(0);
    CallController::Environment asking = environment();
    asking.ask_microphone = [&](std::function<void(bool)> allowed) {
        microphone = std::move(allowed);
    };
    asking.ask_camera = [&](std::function<void(bool)> allowed) { camera = std::move(allowed); };
    asking.enter_call = [] { return std::make_unique<Told>(); };
    asking.open_camera = [opened, open = asking.open_camera](const std::string& device) {
        ++*opened;
        return open(device);
    };
    bob_.setEnvironment(asking);
    bob_.setName("Bob");
    g_calls_updated = 0;

    // On before the room: asked once the microphone is answered, one at a time.
    bob_.setCamera(true);
    both_in();
    ASSERT_TRUE(microphone);
    EXPECT_FALSE(camera) << "asked while the microphone is";
    std::exchange(microphone, {})(true);
    ASSERT_TRUE(camera);

    // Refused: off, said so, never opened.
    std::exchange(camera, {})(false);
    EXPECT_FALSE(bob_.camera());
    EXPECT_FALSE(bob_.notice().isEmpty());
    EXPECT_EQ(*opened, 0);

    // Turned off while asked: that answer is not this turning on's.
    bob_.setCamera(true);
    ASSERT_TRUE(camera);
    bob_.setCamera(false);
    std::exchange(camera, {})(true);
    EXPECT_EQ(*opened, 0);

    // Allowed: opened, the system told it may be seen in the background.
    const int updated = g_calls_updated;
    bob_.setCamera(true);
    ASSERT_TRUE(camera);
    std::exchange(camera, {})(true);
    EXPECT_EQ(*opened, 1);
    EXPECT_EQ(g_calls_updated, updated + 1);
    EXPECT_TRUE(wait_for([&] { return member(ann_, "Bob").value("camera").toBool(); }));
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

TEST_F(CallControllerTest, ASharerShowsAnotherWindowAndTheShareGoesOn) {
    // The desktop gives a screen, then a window of another size.
    CallController::Environment two = environment();
    auto asked = std::make_shared<std::vector<ScreenShareConfig>>();
    two.open_share = [asked](const ScreenShareConfig& config,
                             std::stop_token) -> Result<ScreenShareSources> {
        asked->push_back(config);
        const uint32_t width = asked->size() == 1 ? 320 : 480;
        ScreenShareSources sources{.pictures = std::make_unique<PatternSource>(PatternConfig{
                                       PixelFormat::NV12, width, width * 9 / 16, 30, 1})};
        if (config.sound) {
            sources.sound = std::make_unique<ToneSource>(ToneConfig{.format = {48'000, 2}});
        }
        return sources;
    };
    ann_.setEnvironment(two);
    ann_.setName("Ann");  // the environment's name before
    VideoView own;
    ann_.setSharePreview(&own);
    both_in();
    ann_.share(false, true);
    ASSERT_TRUE(wait_for([&] { return bob_view_.pictureSize() == QSize(320, 180); }));
    EXPECT_TRUE(wait_for([&] { return own.pictureSize() == QSize(320, 180); }))
        << "the sharer sees what it shares";

    ann_.share(true, true);
    EXPECT_TRUE(ann_.choosing());
    EXPECT_TRUE(ann_.sharing()) << "the share goes on while the dialog is open";
    EXPECT_TRUE(wait_for([&] { return bob_view_.pictureSize() == QSize(480, 270); }));
    EXPECT_TRUE(wait_for([&] { return own.pictureSize() == QSize(480, 270); }));
    EXPECT_FALSE(ann_.choosing());
    EXPECT_TRUE(ann_.sharing());
    EXPECT_TRUE(ann_.notice().isEmpty()) << ann_.notice().toStdString();
    EXPECT_EQ(bob_.watching(), id_of(bob_, "Ann")) << "watched throughout";
    ASSERT_EQ(asked->size(), 2u);
    EXPECT_TRUE(asked->at(0).sound);
    EXPECT_TRUE(asked->at(1).window);
    EXPECT_FALSE(asked->at(1).sound) << "the sound shared goes on: none opened beside it";

    ann_.stopSharing();
    EXPECT_FALSE(ann_.sharing());
    EXPECT_TRUE(own.pictureSize().isEmpty()) << "no longer seen";
    EXPECT_TRUE(wait_for([&] { return bob_.watching().isEmpty(); }));
}

TEST_F(CallControllerTest, ASwitchCalledOffLeavesTheShareAsItWas) {
    CallController::Environment once = environment();
    auto opened = std::make_shared<int>(0);
    once.open_share = [opened, open = once.open_share](const ScreenShareConfig& config,
                                                       std::stop_token cancel) {
        if (++*opened > 1) return Result<ScreenShareSources>(fail(Errc::Cancelled));
        return open(config, std::move(cancel));
    };
    ann_.setEnvironment(once);
    ann_.setName("Ann");
    both_in();
    ann_.share(false, false);
    ASSERT_TRUE(wait_for([&] { return bob_view_.pictureSize() == QSize(320, 240); }));
    ann_.share(true, false);
    ASSERT_TRUE(wait_for([&] { return !ann_.choosing(); }));
    EXPECT_TRUE(ann_.sharing());
    EXPECT_TRUE(ann_.notice().isEmpty());
    bob_view_.clear();
    EXPECT_TRUE(wait_for([&] { return bob_view_.pictureSize() == QSize(320, 240); }))
        << "the screen still shared";
}

TEST_F(CallControllerTest, TheShareIsHeardAsLoudAsSetBesideTheVoices) {
    // Bob's speaker, listened to: the loudest it played over a while.
    auto loudest = std::make_shared<std::atomic<float>>(0.0f);
    CallController::Environment bob = environment();
    bob.open_speaker = [loudest](const std::string&) {
        return std::make_unique<VirtualOutput>(
            VirtualOutputConfig{}, [loudest](std::span<const float> samples, Timestamp) {
                float block = 0.0f;
                for (const float sample : samples) block = std::max(block, std::abs(sample));
                float most = loudest->load();
                while (block > most && !loudest->compare_exchange_weak(most, block)) {}
            });
    };
    bob_.setEnvironment(bob);
    bob_.setName("Bob");
    const auto played = [&] {
        wait_for([] { return false; }, 100ms);  // what was pulled before played out
        *loudest = 0.0f;
        wait_for([] { return false; }, 300ms);
        return loudest->load();
    };

    // Set before the room: the share is silent from the start, Ann's voice not.
    bob_.setShareVolume(0.0);
    both_in();
    ann_.setMuted(true);
    ann_.share(false, true);
    ASSERT_TRUE(wait_for([&] { return bob_.watching() == id_of(bob_, "Ann"); }));
    wait_for([] { return false; }, 1s);  // its sound playing by now, as the next shows
    EXPECT_LT(played(), 1e-3f) << "the share silent, Ann's muted voice the least bit";

    bob_.setShareVolume(1.0);
    const float as_it_comes = played();
    EXPECT_GT(as_it_comes, 0.1f);
    bob_.setShareVolume(0.5);
    const float half = played();
    EXPECT_GT(half, as_it_comes * 0.3f);
    EXPECT_LT(half, as_it_comes * 0.7f);

    bob_.setShareVolume(9.0);
    EXPECT_EQ(bob_.shareVolume(), CallController::kMaxVolume);
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

TEST_F(CallControllerTest, PastTheMostCamerasThoseOfTheLastToSpeakAreShown) {
    CallController::Environment one_camera = environment();
    one_camera.call.playback.most_cameras = 1;
    ann_.setEnvironment(std::move(one_camera));
    ann_.setName("Ann");
    // Everyone's microphone a tone: Bob and Carl both talk.
    bob_.setCamera(true);
    carl_.setCamera(true);
    both_in();
    carl_.join(address(ann_));
    const auto shown = [&] {
        QStringList names;
        for (int row = 0; row < members(ann_); ++row) {
            const QVariantMap member = ann_.memberList().get(row);
            if (member.value("cameraShown").toBool()) names << member.value("name").toString();
        }
        return names;
    };
    ASSERT_TRUE(wait_for([&] {
        return member(ann_, "Bob").value("camera").toBool() &&
               member(ann_, "Carl").value("camera").toBool() && shown().size() == 1;
    })) << shown().join(", ").toStdString();
    EXPECT_EQ(ann_.cameras(), 1) << "of the two on, one shown";

    // The one shown falls silent: the other, talking on, is shown after a moment.
    const QString first = shown().value(0);
    (first == "Bob" ? bob_ : carl_).setMuted(true);
    const QString other = first == "Bob" ? "Carl" : "Bob";
    EXPECT_TRUE(wait_for([&] { return shown() == QStringList{other}; }))
        << shown().join(", ").toStdString();
    EXPECT_TRUE(member(ann_, first).value("camera").toBool()) << "on, not shown";
}

/// What a side opened, by the device ids it was given; on the GUI thread.
struct Opened {
    std::vector<std::string> microphones;
    std::vector<std::string> speakers;
    std::vector<std::string> echoes;
    std::vector<std::string> cameras;
};

/// The test's environment, with devices to choose from, kept in `settings_file`,
/// and what it opens told in `opened`.
CallController::Environment with_devices(const QString& settings_file,
                                         std::shared_ptr<Opened> opened) {
    CallController::Environment devices = environment();
    devices.list_microphones = [] {
        return std::vector<AudioDeviceInfo>{{"built-in", "Built-in microphone"},
                                            {"usb", "USB microphone"}};
    };
    devices.list_speakers = [] {
        return std::vector<AudioDeviceInfo>{{"built-in", "Speakers"}, {"headset", "Headset"}};
    };
    devices.list_cameras = [] { return std::vector<CameraInfo>{{1, "front", "Front camera"}}; };
    const auto open_microphone = devices.open_microphone;
    devices.open_microphone = [opened, open_microphone](const std::string& device) {
        opened->microphones.push_back(device);
        return open_microphone(device);
    };
    const auto open_speaker = devices.open_speaker;
    devices.open_speaker = [opened, open_speaker](const std::string& device) {
        opened->speakers.push_back(device);
        return open_speaker(device);
    };
    devices.open_system_sound = [opened](const std::string& device) {
        opened->echoes.push_back(device);
        return std::make_unique<ToneSource>(ToneConfig{.format = {48'000, 2}, .level = 0.0f});
    };
    const auto open_camera = devices.open_camera;
    devices.open_camera = [opened, open_camera](const std::string& device) {
        opened->cameras.push_back(device);
        return open_camera(device);
    };
    devices.settings = [settings_file] {
        return std::make_unique<QSettings>(settings_file, QSettings::IniFormat);
    };
    return devices;
}

TEST_F(CallControllerTest, TheDevicesChosenAreKeptAndUsed) {
    QTemporaryDir kept;
    ASSERT_TRUE(kept.isValid());
    const QString settings_file = kept.filePath("ratcast.conf");
    auto opened = std::make_shared<Opened>();
    ann_.setEnvironment(with_devices(settings_file, opened));
    EXPECT_TRUE(ann_.microphoneDevice().isEmpty()) << "the default";

    ann_.listDevices();
    ASSERT_EQ(ann_.microphoneDevices().size(), 2);
    EXPECT_EQ(ann_.microphoneDevices()[1].toMap().value("name"), "USB microphone");
    EXPECT_EQ(ann_.speakerDevices().size(), 2);
    EXPECT_EQ(ann_.cameraDevices().value(0).toMap().value("id"), "front");
    ann_.setMicrophoneDevice("usb");
    ann_.setSpeakerDevice("headset");
    ann_.setCameraDevice("front");

    // Another run: the same choices, and the camera chosen not there now —
    // listed all the same, by the name it had.
    CallController::Environment later = with_devices(settings_file, opened);
    later.list_cameras = [] { return std::vector<CameraInfo>{{2, "back", "Back camera"}}; };
    carl_.setEnvironment(later);
    EXPECT_EQ(carl_.microphoneDevice(), "usb");
    EXPECT_EQ(carl_.speakerDevice(), "headset");
    EXPECT_EQ(carl_.cameraDevice(), "front");
    carl_.listDevices();
    ASSERT_EQ(carl_.cameraDevices().size(), 2);
    const QVariantMap missing = carl_.cameraDevices()[1].toMap();
    EXPECT_EQ(missing.value("id"), "front");
    EXPECT_EQ(missing.value("name"), "Front camera");
    EXPECT_TRUE(missing.value("missing").toBool());

    carl_.setCamera(true);
    carl_.create();
    ASSERT_EQ(carl_.state(), State::InRoom);
    EXPECT_EQ(opened->microphones, std::vector<std::string>{"usb"});
    EXPECT_EQ(opened->speakers, std::vector<std::string>{"headset"});
    EXPECT_EQ(opened->echoes, std::vector<std::string>{"headset"}) << "what it plays, the echo";
    EXPECT_EQ(opened->cameras, std::vector<std::string>{"front"});

    // The default again: nothing kept.
    carl_.setMicrophoneDevice("");
    carl_.setSpeakerDevice("");
    carl_.setCameraDevice("");
    EXPECT_TRUE(QSettings(settings_file, QSettings::IniFormat).childGroups().isEmpty());
}

TEST_F(CallControllerTest, ADeviceChosenInARoomIsUsedAtOnce) {
    QTemporaryDir kept;
    ASSERT_TRUE(kept.isValid());
    auto opened = std::make_shared<Opened>();
    CallController::Environment ann = with_devices(kept.filePath("ratcast.conf"), opened);
    // The headset, listened to: the loudest it played over a while.
    auto loudest = std::make_shared<std::atomic<float>>(0.0f);
    ann.open_speaker = [opened, loudest](const std::string& device) {
        opened->speakers.push_back(device);
        VirtualOutput::Tap tap;
        if (device == "headset") {
            tap = [loudest](std::span<const float> samples, Timestamp) {
                float block = 0.0f;
                for (const float sample : samples) block = std::max(block, std::abs(sample));
                float most = loudest->load();
                while (block > most && !loudest->compare_exchange_weak(most, block)) {}
            };
        }
        return std::make_unique<VirtualOutput>(VirtualOutputConfig{}, tap);
    };
    ann_.setEnvironment(ann);
    ann_.setName("Ann");
    ann_.setCamera(true);
    both_in();
    ASSERT_TRUE(wait_for([&] {
        return member(bob_, "Ann").value("speaking").toBool() &&
               member(bob_, "Ann").value("camera").toBool();
    }));

    ann_.setSpeakerDevice("headset");
    EXPECT_EQ(opened->speakers, (std::vector<std::string>{"", "headset"}));
    EXPECT_EQ(opened->echoes, (std::vector<std::string>{"", "headset"})) << "the echo follows";
    EXPECT_TRUE(wait_for([&] { return loudest->load() > 0.1f; })) << "Bob heard on the headset";

    ann_.setMicrophoneDevice("usb");
    ann_.setCameraDevice("front");
    EXPECT_EQ(opened->microphones.back(), "usb");
    EXPECT_EQ(opened->cameras, (std::vector<std::string>{"", "front"}));
    EXPECT_TRUE(ann_.camera());
    EXPECT_TRUE(ann_.notice().isEmpty()) << ann_.notice().toStdString();
    // Heard and seen as before, from the devices chosen: once what was
    // heard from the ones before has gone by.
    wait_for([] { return false; }, 1500ms);
    EXPECT_TRUE(wait_for([&] {
        return member(bob_, "Ann").value("speaking").toBool() &&
               member(bob_, "Ann").value("camera").toBool();
    }));
}

TEST_F(CallControllerTest, WithoutACameraItIsTurnedOffAndTheRoomGoesOn) {
    CallController::Environment blind = environment();
    blind.open_camera = [](const std::string&) { return std::unique_ptr<VideoSource>{}; };
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

// A link that names a server has whoever joins by it meet there, though it
// named none itself; one whose server is no server is no link.
TEST_F(CallControllerTest, ALinksServerIsMetAtByWhoeverJoinsByIt) {
    librats::NodeConfig node;
    node.bind_address = "127.0.0.1";
    node.protocol = environment().call.protocol;
    librats::Node server(node);
    librats::Rendezvous::Config serving;
    serving.serve = true;
    server.add_subsystem(std::make_unique<librats::Rendezvous>(serving));
    ASSERT_TRUE(server.start());

    const QString room = make_room();
    bob_.join(room + "?via=not a server:");
    EXPECT_EQ(bob_.state(), State::Idle);
    EXPECT_FALSE(bob_.notice().isEmpty());
    bob_.join(room + QString("?via=127.0.0.1:%1").arg(server.listen_port()));
    // Bob is met there; Ann may be too, by then, having heard of it from him.
    const auto at_server = [&](const QString& id) {
        for (const librats::PeerInfo& peer : server.peers())
            if (QString::fromStdString(peer.id.to_hex()) == id) return true;
        return false;
    };
    ASSERT_TRUE(wait_for([&] { return members(ann_) == 1 && at_server(id_of(ann_, "Bob")); }));
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
    // Not the desktop's last choice kept: that would stand for the dialog
    // from then on.
    CallController::Environment kept = environment();
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

// Members known by whose they are: a tag beside the name, asked to be friends
// from the room, a friend once accepted, heard as loud as their person is set
// to be — whichever device or room they come in.
/// A stand-in server home nodes meet at, and each person's home node there.
class PeopleAtAServer {
public:
    PeopleAtAServer() : server_(node_config()) {
        librats::Rendezvous::Config serving;
        serving.serve = true;
        server_.add_subsystem(std::make_unique<librats::Rendezvous>(serving));
        EXPECT_TRUE(server_.start());
        EXPECT_TRUE(data_.isValid());
    }
    ~PeopleAtAServer() { server_.stop(); }

    /// The server as CallController::server takes it.
    QString server() const { return QStringLiteral("127.0.0.1:%1").arg(server_.listen_port()); }
    /// `side`'s rooms met at the server.
    void meet(CallController& side) const {
        CallController::Environment env = environment();
        env.server = server();
        env.name = side.name();
        side.setEnvironment(env);
    }

    void start(PeopleController& people, const QString& name) {
        PeopleController::Environment env;
        env.people.data_dir = QDir(data_.path()).filePath(name).toStdString();
        env.people.servers = {{"127.0.0.1", server_.listen_port()}};
        env.people.bind_address = "127.0.0.1";
        env.people.protocol = environment().call.protocol;
        env.people.watch_network = false;
        env.people.dht = false;  // the server alone, as the room
        env.name = name;
        people.setEnvironment(env);
        ASSERT_TRUE(people.ready());
    }

private:
    static librats::NodeConfig node_config() {
        librats::NodeConfig node;
        node.bind_address = "127.0.0.1";
        node.protocol = environment().call.protocol;
        return node;
    }
    librats::Node server_;
    QTemporaryDir data_;
};

TEST_F(CallControllerTest, AMemberIsKnownByWhoseItIsAndAskedToBeAFriend) {
    PeopleAtAServer at;
    PeopleController ann_people;
    PeopleController bob_people;
    at.start(ann_people, "Ann");
    at.start(bob_people, "Bob");
    at.meet(ann_);  // the room rung to met by its secret
    at.meet(bob_);
    ann_.setPeople(&ann_people);
    bob_.setPeople(&bob_people);
    EXPECT_EQ(bob_.name(), "Bob") << "the person's name";

    both_in();
    ASSERT_TRUE(wait_for([&] { return member(bob_, "Ann").value("tag") == ann_people.tag(); }));
    QVariantMap ann = member(bob_, "Ann");
    EXPECT_EQ(ann.value("person"), ann_people.person());
    EXPECT_TRUE(ann.value("befriendable").toBool());
    EXPECT_FALSE(ann.value("isFriend").toBool());

    // Each told as it goes, without looking at the list.
    QStringList told_bob;
    QStringList asking_ann;
    QObject::connect(&bob_people, &PeopleController::friendNews, &bob_people,
                     [&](const QString& notice, const QString&) { told_bob << notice; });
    QObject::connect(&ann_people, &PeopleController::friendNews, &ann_people,
                     [&](const QString&, const QString& asking) { asking_ann << asking; });

    bob_.addFriend(ann.value("peerId").toString());
    EXPECT_TRUE(wait_for([&] { return !member(bob_, "Ann").value("befriendable").toBool(); }))
        << "asked already";
    EXPECT_EQ(told_bob.size(), 1) << "told it was asked, at once";
    ASSERT_TRUE(wait_for([&] { return ann_people.asking() == 1; }));
    ASSERT_TRUE(wait_for([&] { return !asking_ann.isEmpty(); }));
    EXPECT_EQ(asking_ann.front(), bob_people.person()) << "answered from the notice";
    ann_people.accept(bob_people.person());
    EXPECT_TRUE(wait_for([&] { return member(bob_, "Ann").value("isFriend").toBool(); }));
    EXPECT_TRUE(wait_for([&] { return member(ann_, "Bob").value("isFriend").toBool(); }));
    EXPECT_TRUE(wait_for([&] { return told_bob.size() == 2; }));
    EXPECT_TRUE(told_bob.back().contains("Ann")) << told_bob.back().toStdString();

    // How loud Ann is heard is kept for her person, not this node of hers.
    bob_.setVolume(ann.value("peerId").toString(), 0.5);
    EXPECT_TRUE(wait_for([&] { return member(bob_, "Ann").value("volume").toDouble() == 0.5; }));
    bob_.leave();
    ann_.leave();
    ann_.create();
    ASSERT_EQ(ann_.state(), State::InRoom);
    bob_.join(ann_.invites().value(0));
    ASSERT_TRUE(wait_for([&] { return member(bob_, "Ann").value("tag") == ann_people.tag(); }));
    EXPECT_NE(member(bob_, "Ann").value("peerId"), ann.value("peerId")) << "another node of hers";
    EXPECT_EQ(member(bob_, "Ann").value("volume").toDouble(), 0.5);
    bob_.leave();
    ann_.leave();

    // Friends seen online and in a room; one rung joins the other's room.
    const auto friend_of = [](PeopleController& people) {
        const QVariantList friends = people.friends();
        return friends.isEmpty() ? QVariantMap{} : friends.front().toMap();
    };
    ASSERT_TRUE(wait_for([&] { return friend_of(ann_people).value("online").toBool(); }));
    bob_.ring(ann_people.person());
    ASSERT_EQ(bob_.state(), State::InRoom) << "a room made to ring to";
    EXPECT_EQ(bob_people.ringing().value("name"), "Ann");
    EXPECT_TRUE(wait_for([&] { return friend_of(ann_people).value("inRoom").toBool(); }));
    ASSERT_TRUE(wait_for([&] { return ann_people.rings().size() == 1; }));
    const QVariantMap ring = ann_people.rings().front().toMap();
    EXPECT_EQ(ring.value("name"), "Bob");
    ann_.acceptRing(ring.value("id").toString());
    EXPECT_NE(ann_.state(), State::Idle);
    EXPECT_TRUE(wait_for([&] { return member(bob_, "Ann").value("tag") == ann_people.tag(); }));
    EXPECT_TRUE(wait_for([&] { return bob_people.ringing().isEmpty(); })) << "still ringing";
    EXPECT_TRUE(ann_people.rings().isEmpty());
    bob_.leave();
    ann_.leave();
    ann_.setPeople(nullptr);
    bob_.setPeople(nullptr);
}

// From the list while in a room: another group's entered in place of it, its
// own a click that changes nothing.
TEST_F(CallControllerTest, AnotherGroupsRoomIsEnteredFromARoom) {
    PeopleAtAServer at;
    PeopleController people;
    at.start(people, "Ann");
    at.meet(ann_);
    ann_.setPeople(&people);

    const QString films = people.makeGroup("Films");
    const QString games = people.makeGroup("Games");
    ann_.create();
    ASSERT_EQ(ann_.state(), State::InRoom);
    ann_.enter(films);
    ASSERT_EQ(ann_.state(), State::InRoom);
    EXPECT_EQ(ann_.group(), films) << "a room of nobody's left for it";

    const QStringList invites = ann_.invites();
    ann_.enter(films);
    EXPECT_EQ(ann_.invites(), invites) << "not left and made again";

    ann_.enter("not a group");
    EXPECT_EQ(ann_.group(), films) << "stayed: nowhere to go";
    ann_.enter(games);
    ASSERT_EQ(ann_.state(), State::InRoom);
    EXPECT_EQ(ann_.group(), games);
    ann_.leave();
}

// A group made is entered at once, its link the room's invite; whoever opens
// the link is in its room too, the group theirs from then on, and sees from
// the list who is there.
TEST_F(CallControllerTest, AGroupsRoomIsEnteredFromTheListAndByItsLink) {
    PeopleAtAServer at;
    PeopleController ann_people;
    PeopleController bob_people;
    at.start(ann_people, "Ann");
    at.start(bob_people, "Bob");
    at.meet(ann_);
    at.meet(bob_);
    ann_.setPeople(&ann_people);
    bob_.setPeople(&bob_people);

    const QString id = ann_people.makeGroup("Films");
    ASSERT_FALSE(id.isEmpty());
    ASSERT_EQ(ann_people.groups().size(), 1);
    const QVariantMap group = ann_people.groups().front().toMap();
    EXPECT_EQ(group.value("name"), "Films");
    EXPECT_TRUE(PeopleController::isGroupLink(group.value("link").toString()));
    ann_.enter(id);
    ASSERT_EQ(ann_.state(), State::InRoom) << "nobody's room, there at once";
    EXPECT_EQ(ann_.group(), id);
    ASSERT_FALSE(ann_.invites().isEmpty());
    EXPECT_EQ(ann_.invites().front(), group.value("link")) << "its link the room's";

    bob_.join(ann_.invites().front());
    EXPECT_EQ(bob_.state(), State::InRoom);
    EXPECT_EQ(bob_.group(), id);
    ASSERT_TRUE(
        wait_for([&] { return !member(bob_, "Ann").isEmpty() && !member(ann_, "Bob").isEmpty(); }));
    const auto bobs = [&] {
        const QVariantList groups = bob_people.groups();
        return groups.isEmpty() ? QVariantMap{} : groups.front().toMap();
    };
    EXPECT_TRUE(wait_for([&] { return bobs().value("name") == "Films"; })) << "told in the room";

    // Seen from the list: Ann in it, Bob out.
    bob_.leave();
    EXPECT_EQ(bob_.group(), "");
    EXPECT_TRUE(wait_for([&] { return bobs().value("present").toInt() == 1; }));
    // Who it is, told by her home node; and, as she shares, a notice.
    const auto here = [&] { return bobs().value("here").toList(); };
    EXPECT_TRUE(wait_for([&] { return here().size() == 1; }));
    EXPECT_EQ(here().value(0).toMap().value("name"), "Ann");
    EXPECT_FALSE(here().value(0).toMap().value("sharing").toBool());
    QStringList told;
    QObject::connect(&bob_people, &PeopleController::shares,
                     [&](const QString& group, const QString& name, const QString& who) {
                         told << group + " " + name + " " + who;
                     });
    ann_.share(false, false);
    ASSERT_TRUE(wait_for([&] { return ann_.sharing(); })) << ann_.notice().toStdString();
    EXPECT_TRUE(wait_for([&] { return here().value(0).toMap().value("sharing").toBool(); }));
    EXPECT_TRUE(wait_for([&] { return told.size() == 1; }));
    EXPECT_EQ(told.value(0), id + " Films Ann");
    bob_.enter(id);
    EXPECT_EQ(bob_.state(), State::InRoom);
    EXPECT_TRUE(wait_for([&] { return !member(bob_, "Ann").isEmpty(); }));
    bob_.leave();
    ann_.leave();
    EXPECT_TRUE(wait_for([&] { return bobs().value("present").toInt() == 0; }));
    ann_.setPeople(nullptr);
    bob_.setPeople(nullptr);
}

// A group's link reset while its members are in its room: the room moves
// with it, each of them there again.
TEST_F(CallControllerTest, AGroupsRoomMovesWithItsLinkReset) {
    PeopleAtAServer at;
    PeopleController ann_people;
    PeopleController bob_people;
    at.start(ann_people, "Ann");
    at.start(bob_people, "Bob");
    at.meet(ann_);
    at.meet(bob_);
    ann_.setPeople(&ann_people);
    bob_.setPeople(&bob_people);

    const QString id = ann_people.makeGroup("Films");
    ann_.enter(id);
    ASSERT_EQ(ann_.state(), State::InRoom);
    const QString old = ann_.invites().value(0);
    bob_.join(old);
    ASSERT_TRUE(
        wait_for([&] { return !member(bob_, "Ann").isEmpty() && !member(ann_, "Bob").isEmpty(); }));

    ann_people.resetGroupLink(id);
    EXPECT_TRUE(wait_for([&] {
        return ann_.state() == State::InRoom && !ann_.invites().isEmpty() &&
               ann_.invites().front() != old;
    }));
    EXPECT_EQ(ann_.group(), id);
    EXPECT_TRUE(wait_for([&] {
        return bob_.state() == State::InRoom && bob_.invites().value(0) == ann_.invites().value(0);
    })) << bob_.invites().value(0).toStdString();
    EXPECT_TRUE(
        wait_for([&] { return !member(bob_, "Ann").isEmpty() && !member(ann_, "Bob").isEmpty(); }));
    bob_.leave();
    ann_.leave();
    ann_.setPeople(nullptr);
    bob_.setPeople(nullptr);
}

// A room is made once: its link is its secret, and the next room's another —
// whoever had this one's is not let into that.
TEST_F(CallControllerTest, EachRoomMadeHasALinkOfItsOwn) {
    ann_.create();
    ASSERT_EQ(ann_.state(), State::InRoom);
    const QString first = ann_.invites().value(0);
    const std::optional<RatsLink> link = parse_rats_link(first.toStdString());
    ASSERT_TRUE(link) << first.toStdString();
    EXPECT_TRUE(std::holds_alternative<RoomSecret>(link->url)) << first.toStdString();
    ann_.leave();
    ann_.create();
    ASSERT_EQ(ann_.state(), State::InRoom);
    EXPECT_NE(ann_.invites().value(0), first);
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
