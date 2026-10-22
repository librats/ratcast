// The whole path the tools take, in one process: a pattern source, the
// publisher, MediaService on a librats node, loopback UDP, MediaService on
// another node, the player. Stamps in the decoded pictures prove each one
// made the trip.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ratcast/client/video_player.h"
#include "ratcast/client/video_publisher.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/session/media_service.h"

#include "librats/node/node.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

librats::NodeConfig config(bool listen) {
    librats::NodeConfig c;
    c.listen_port = 0;
    c.bind_address = "127.0.0.1";
    c.enable_listen = listen;
    c.enable_network_monitor = false;
    c.protocol = "ratcast-test/client-over-librats";
    return c;
}

}  // namespace

TEST(ClientOverLibrats, StreamsAPatternBetweenTwoNodes) {
    VideoEncoderConfig probe;
    probe.width = 640;
    probe.height = 360;
    if (!open_video_encoder(probe)) GTEST_SKIP() << "no AV1 encoder here";

    // Sender: the publisher answers the session's keyframe requests.
    librats::Node sender_node(config(true));
    std::atomic<VideoPublisher*> publisher{nullptr};
    auto* sender = sender_node.add_subsystem(std::make_unique<MediaService>(SessionHandlers{
        .keyframe_needed =
            [&](StreamId) {
                if (VideoPublisher* p = publisher.load()) p->request_keyframe();
            },
    }));

    // Receiver: a player for the first stream announced.
    std::mutex mutex;
    std::vector<Timestamp> stamps;
    std::unique_ptr<VideoPlayer> player;
    std::atomic<VideoPlayer*> playing{nullptr};
    librats::Node receiver_node(config(false));
    MediaService* receiver = nullptr;
    receiver = receiver_node.add_subsystem(std::make_unique<MediaService>(SessionHandlers{
        .stream_announced =
            [&](const PeerId& peer, const StreamDescription& description) {
                std::lock_guard lock(mutex);
                if (player) return;
                player = std::make_unique<VideoPlayer>(
                    receiver->session(), peer, description, [&](VideoFrame picture) {
                        const std::optional<Timestamp> stamp = read_timestamp(picture);
                        std::lock_guard lock(mutex);
                        if (stamp) stamps.push_back(*stamp);
                    });
                EXPECT_TRUE(player->start());
                playing = player.get();
            },
        .frame_received =
            [&](const PeerId&, EncodedFrame frame) {
                if (VideoPlayer* p = playing.load()) p->push(std::move(frame));
            },
    }));

    PatternSource pattern({PixelFormat::NV12, 640, 360, 60, 1});
    VideoPublisherConfig settings;
    settings.encoder.bitrate = 2'000'000;
    VideoPublisher video(sender->session(), settings);
    publisher = &video;

    ASSERT_TRUE(sender_node.start());
    ASSERT_TRUE(receiver_node.start());
    ASSERT_TRUE(video.start(pattern));
    receiver_node.connect("127.0.0.1", sender_node.listen_port());

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    const auto received = [&] {
        std::lock_guard lock(mutex);
        return stamps.size();
    };
    while (received() < 60 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }

    video.stop();
    publisher = nullptr;
    playing = nullptr;
    VideoPlayer* started = nullptr;
    {
        std::lock_guard lock(mutex);
        started = player.get();
    }
    if (started) started->stop();  // not under the lock: its sink takes it
    receiver_node.stop();
    sender_node.stop();

    std::lock_guard lock(mutex);
    ASSERT_GE(stamps.size(), 60u) << "a second of 60 fps did not arrive in ten";
    for (size_t i = 1; i < stamps.size(); ++i) EXPECT_GE(stamps[i], stamps[i - 1]);
    EXPECT_EQ(player->stats().decode_errors, 0u);
}
