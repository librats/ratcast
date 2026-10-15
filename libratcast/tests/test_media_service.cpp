// MediaService on two real librats nodes over loopback: the session's messages
// on the Media lane, frames as live messages, peer events from the node.

#include "ratcast/session/media_service.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "librats/node/node.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

// A protocol id of its own per test, so a node from a test running alongside
// that dials a reused port fails the handshake instead of joining this one.
librats::NodeConfig config(bool listen) {
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    librats::NodeConfig c;
    c.listen_port = 0;
    c.bind_address = "127.0.0.1";
    c.enable_listen = listen;
    c.enable_network_monitor = false;
    c.protocol = std::string("ratcast-test/") + test->test_suite_name() + "." + test->name();
    return c;
}

StreamDescription screen() {
    StreamDescription d;
    d.id = 1;
    d.codec = Codec::AV1;
    d.format = VideoFormat{1920, 1080, 60, 1};
    d.lifetime = 500ms;
    d.label = "screen";
    return d;
}

uint8_t pattern(FrameNumber number, size_t i) { return static_cast<uint8_t>(number * 7 + i); }

/// Everything the receiving side's handlers saw, safe to read from the test.
struct Received {
    std::mutex mutex;
    std::vector<FrameNumber> frames;
    bool payloads_intact = true;
    std::atomic<bool> announced{false};
    std::atomic<bool> withdrawn{false};

    size_t count() {
        std::lock_guard lock(mutex);
        return frames.size();
    }
};

std::string describe(const OutgoingStreamStats& s) {
    return "sent: frames " + std::to_string(s.frames) + ", messages " + std::to_string(s.messages) +
           ", awaiting keyframe " + std::to_string(s.awaiting_keyframe) + ", backpressure drops " +
           std::to_string(s.backpressure_drops) + ", keyframes asked " +
           std::to_string(s.keyframes_asked);
}

std::string describe(const IncomingStreamStats& s) {
    const FrameTracker::Stats& f = s.frames;
    return "received: frames " + std::to_string(f.received) + ", delivered " +
           std::to_string(f.delivered) + ", late " + std::to_string(f.late) + ", superseded " +
           std::to_string(f.superseded) + ", undecodable " + std::to_string(f.undecodable) +
           ", lost " + std::to_string(f.lost) + ", keyframe requests " +
           std::to_string(f.keyframe_requests);
}

}  // namespace

TEST(MediaService, StreamsFramesBetweenTwoNodes) {
    std::atomic<bool> keyframe_needed{false};
    librats::Node sender_node(config(true));
    auto* sender = sender_node.add_subsystem(std::make_unique<MediaService>(SessionHandlers{
        .keyframe_needed = [&](StreamId) { keyframe_needed = true; },
    }));

    Received received;
    librats::Node receiver_node(config(false));
    MediaService* receiver = nullptr;
    receiver = receiver_node.add_subsystem(std::make_unique<MediaService>(SessionHandlers{
        .stream_announced =
            [&](const PeerId& peer, const StreamDescription& d) {
                received.announced = true;
                EXPECT_TRUE(receiver->session().subscribe(peer, d.id));  // from a network thread
            },
        .stream_withdrawn = [&](const PeerId&, StreamId) { received.withdrawn = true; },
        .frame_received =
            [&](const PeerId&, EncodedFrame frame) {
                std::lock_guard lock(received.mutex);
                received.frames.push_back(frame.info.number);
                for (size_t i = 0; i < frame.payload.size(); ++i) {
                    if (frame.payload.data()[i] != pattern(frame.info.number, i)) {
                        received.payloads_intact = false;
                    }
                }
            },
    }));

    ASSERT_TRUE(sender->session().add_stream(screen()));
    ASSERT_TRUE(sender_node.start());
    ASSERT_TRUE(receiver_node.start());
    receiver_node.connect("127.0.0.1", sender_node.listen_port());

    // Announced on connect, subscribed from the handler, keyframe asked for.
    ASSERT_TRUE(wait_for([&] { return sender->session().outgoing_stats(1)->subscribers == 1; }));
    ASSERT_TRUE(received.announced);
    ASSERT_TRUE(wait_for([&] { return keyframe_needed.load(); }));

    // An encoder at work: a keyframe when asked for, deltas otherwise, frames of
    // every size up to a large keyframe.
    FrameNumber next = 0;
    const auto send = [&](bool keyframe) {
        const FrameNumber n = next++;
        keyframe = keyframe || keyframe_needed.exchange(false);
        const size_t size = keyframe ? 256 * 1024 : 1000 + (n * 997) % 20'000;
        MutableBuffer payload = allocate_frame_payload(size);
        uint8_t* bytes = payload.append(size);
        for (size_t i = 0; i < size; ++i) bytes[i] = pattern(n, i);

        FrameInfo info;
        info.number = n;
        info.keyframe = keyframe;
        info.capture_time = SteadyClock::instance().now();
        if (!keyframe) info.references = {n - 1};
        return sender->session().send_frame(1, info, std::move(payload));
    };
    const auto last_received = [&]() -> std::optional<FrameNumber> {
        std::lock_guard lock(received.mutex);
        if (received.frames.empty()) return std::nullopt;
        return received.frames.back();
    };

    constexpr FrameNumber kFrames = 120;
    while (next < kFrames) {
        ASSERT_TRUE(send(next % 60 == 0));
        std::this_thread::sleep_for(2ms);
    }
    // The stream reaches its end. Live messages may expire on a machine this
    // busy even over loopback — what the receiver does then is the session
    // tests' business — so a few keyframes follow, in case the last frames were
    // among them.
    ASSERT_TRUE(wait_for([&] {
        const std::optional<FrameNumber> last = last_received();
        if (last && *last >= kFrames - 1) return true;
        if (next < kFrames + 10) {
            EXPECT_TRUE(send(true));
        }
        std::this_thread::sleep_for(50ms);
        return false;
    })) << describe(*sender->session().outgoing_stats(1))
        << "\n"
        << describe(*receiver->session().incoming_stats(sender_node.local_id(), 1));

    // Whatever arrived arrived in decode order, intact.
    {
        std::lock_guard lock(received.mutex);
        EXPECT_TRUE(std::ranges::adjacent_find(received.frames, std::greater_equal<>{}) ==
                    received.frames.end());
        EXPECT_TRUE(received.payloads_intact);
    }

    // Withdrawing reaches the receiver; disconnecting reaches the sender.
    sender->session().remove_stream(1);
    ASSERT_TRUE(wait_for([&] { return received.withdrawn.load(); }));

    ASSERT_TRUE(sender->session().add_stream(screen()));
    ASSERT_TRUE(wait_for([&] { return sender->session().outgoing_stats(1)->subscribers == 1; }));
    receiver_node.stop();
    ASSERT_TRUE(wait_for([&] { return sender->session().outgoing_stats(1)->subscribers == 0; }));
    sender_node.stop();
}
