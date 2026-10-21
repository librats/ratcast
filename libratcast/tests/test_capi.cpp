// The C API (ratcast/ratcast.h) on two real librats nodes over loopback,
// reached through C calls only: rats.h for the nodes, ratcast.h for media.

#include "ratcast/ratcast.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ratcast/version.h"

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

/// A protocol id of its own per test, so a node from a test running alongside
/// that dials a reused port fails the handshake instead of joining this one.
std::string protocol() {
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    return std::string("ratcast-test/") + test->test_suite_name() + "." + test->name();
}

rats_t make_node(const std::string& protocol, bool listen) {
    rats_config_t config{};
    config.protocol = protocol.c_str();
    config.bind_address = "127.0.0.1";
    config.dial_only = listen ? 0 : 1;
    config.disable_network_monitor = 1;
    return rats_create(&config);
}

/// Everything a node's handlers saw, safe to read from the test.
struct Seen {
    std::mutex mutex;
    std::string announced_peer;
    std::vector<uint32_t> announced;
    std::string label;
    std::vector<uint8_t> codec_config;
    ratcast_stream_t stream{};  ///< the last announced, its pointers cleared
    std::vector<ratcast_frame_t> frames;
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<uint32_t> withdrawn;
    size_t frames_when_withdrawn = 0;
    std::atomic<int> keyframes_needed{0};
    /// The frame handler unsubscribes when this frame comes.
    ratcast_t media = nullptr;
    uint64_t unsubscribe_at = UINT64_MAX;

    size_t frame_count() {
        std::lock_guard lock(mutex);
        return frames.size();
    }
    size_t announced_count() {
        std::lock_guard lock(mutex);
        return announced.size();
    }
    size_t withdrawn_count() {
        std::lock_guard lock(mutex);
        return withdrawn.size();
    }
};

ratcast_handlers_t handlers(Seen& seen) {
    ratcast_handlers_t h{};
    h.user = &seen;
    h.stream_announced = [](void* user, const char* peer, const ratcast_stream_t* stream) {
        auto& s = *static_cast<Seen*>(user);
        std::lock_guard lock(s.mutex);
        s.announced_peer = peer;
        s.announced.push_back(stream->id);
        s.label = stream->label;
        const auto* config = static_cast<const uint8_t*>(stream->codec_config);
        s.codec_config.assign(config, config + stream->codec_config_size);
        s.stream = *stream;
        s.stream.label = nullptr;
        s.stream.codec_config = nullptr;
    };
    h.stream_withdrawn = [](void* user, const char*, uint32_t stream) {
        auto& s = *static_cast<Seen*>(user);
        std::lock_guard lock(s.mutex);
        s.withdrawn.push_back(stream);
        s.frames_when_withdrawn = s.frames.size();
    };
    h.frame_received = [](void* user, const char* peer, const ratcast_frame_t* frame) {
        auto& s = *static_cast<Seen*>(user);
        std::lock_guard lock(s.mutex);
        s.frames.push_back(*frame);
        s.frames.back().data = nullptr;
        const auto* bytes = static_cast<const uint8_t*>(frame->data);
        s.payloads.emplace_back(bytes, bytes + frame->size);
        if (frame->number == s.unsubscribe_at) {
            EXPECT_EQ(ratcast_unsubscribe(s.media, peer, frame->stream), RATCAST_OK);
        }
    };
    h.keyframe_needed = [](void* user, uint32_t) { ++static_cast<Seen*>(user)->keyframes_needed; };
    return h;
}

ratcast_stream_t screen() {
    ratcast_stream_t s{};
    s.id = 1;
    s.codec = RATCAST_CODEC_AV1;
    s.video.width = 1920;
    s.video.height = 1080;
    s.video.frame_rate_num = 60;
    s.lifetime_us = 500'000;
    s.label = "screen";
    return s;
}

ratcast_stream_t feed() {
    ratcast_stream_t s{};
    s.id = 3;
    s.codec = RATCAST_CODEC_MPEG_TS;
    s.lifetime_us = 200'000;
    s.label = "ts";
    return s;
}

std::vector<uint8_t> payload(uint64_t number, size_t size) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(number * 7 + i);
    return bytes;
}

ratcast_frame_t frame_of(uint32_t stream, uint64_t number, const std::vector<uint8_t>& bytes) {
    ratcast_frame_t f{};
    f.stream = stream;
    f.number = number;
    f.capture_time_us = ratcast_now_us();
    f.keyframe = 1;
    f.data = bytes.data();
    f.size = bytes.size();
    return f;
}

/// A sender and a viewer, each a node with ratcast attached, connected.
class CApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        sender_node_ = make_node(protocol_, true);
        viewer_node_ = make_node(protocol_, false);
        ASSERT_NE(sender_node_, nullptr);
        ASSERT_NE(viewer_node_, nullptr);
        const ratcast_handlers_t sender_handlers = handlers(sender_seen_);
        const ratcast_handlers_t viewer_handlers = handlers(viewer_seen_);
        ASSERT_EQ(ratcast_attach(sender_node_, &sender_handlers, nullptr, &sender_), RATCAST_OK);
        ASSERT_EQ(ratcast_attach(viewer_node_, &viewer_handlers, nullptr, &viewer_), RATCAST_OK);
    }

    void TearDown() override {
        rats_destroy(viewer_node_);
        rats_destroy(sender_node_);
    }

    void connect() {
        ASSERT_EQ(rats_start(sender_node_), RATCAST_OK);
        ASSERT_EQ(rats_start(viewer_node_), RATCAST_OK);
        ASSERT_EQ(rats_connect(viewer_node_, "127.0.0.1", rats_listen_port(sender_node_)), RATS_OK);
        ASSERT_TRUE(wait_for([&] { return rats_peer_count(viewer_node_) == 1; }));
        sender_id_ = rats_local_id(sender_node_);
    }

    const std::string protocol_ = protocol();
    rats_t sender_node_ = nullptr;
    rats_t viewer_node_ = nullptr;
    ratcast_t sender_ = nullptr;
    ratcast_t viewer_ = nullptr;
    Seen sender_seen_;
    Seen viewer_seen_;
    std::string sender_id_;
};

TEST_F(CApiTest, AttachingHasItsRules) {
    ratcast_t again = nullptr;
    EXPECT_EQ(ratcast_attach(sender_node_, nullptr, nullptr, &again), RATCAST_ERR_EXISTS);
    EXPECT_EQ(ratcast_attach(nullptr, nullptr, nullptr, &again), RATCAST_ERR_INVALID_ARG);
    EXPECT_EQ(again, nullptr);

    rats_t late = make_node(protocol_, false);
    ASSERT_EQ(rats_start(late), RATS_OK);
    EXPECT_EQ(ratcast_attach(late, nullptr, nullptr, &again), RATCAST_ERR_ALREADY_STARTED);
    rats_destroy(late);

    // No handlers and a config: all defaults.
    rats_t bare = make_node(protocol_, false);
    ratcast_config_t config{};
    config.keyframe_retry_us = 300'000;
    ASSERT_EQ(ratcast_attach(bare, nullptr, &config, &again), RATCAST_OK);
    const ratcast_stream_t stream = screen();
    EXPECT_EQ(ratcast_add_stream(again, &stream, nullptr), RATCAST_OK);
    ASSERT_EQ(rats_start(bare), RATS_OK);
    rats_destroy(bare);
}

TEST_F(CApiTest, AStreamThatIsNotValidIsRefused) {
    const auto refused = [&](void (*change)(ratcast_stream_t&)) {
        ratcast_stream_t s = screen();
        change(s);
        return ratcast_add_stream(sender_, &s, nullptr);
    };
    EXPECT_EQ(refused([](ratcast_stream_t& s) { s.lifetime_us = 0; }), RATCAST_ERR_INVALID_ARG);
    EXPECT_EQ(refused([](ratcast_stream_t& s) { s.lifetime_us = RATCAST_MAX_LIFETIME_US + 1; }),
              RATCAST_ERR_INVALID_ARG);
    EXPECT_EQ(refused([](ratcast_stream_t& s) { s.video.width = 0; }), RATCAST_ERR_INVALID_ARG);
    EXPECT_EQ(refused([](ratcast_stream_t& s) { s.codec = static_cast<ratcast_codec_t>(0); }),
              RATCAST_ERR_INVALID_ARG);
    EXPECT_EQ(refused([](ratcast_stream_t& s) { s.codec = static_cast<ratcast_codec_t>(256); }),
              RATCAST_ERR_INVALID_ARG);
    EXPECT_EQ(refused([](ratcast_stream_t& s) { s.codec = static_cast<ratcast_codec_t>(200); }),
              RATCAST_ERR_INVALID_ARG)
        << "a codec not named needs its kind";
    EXPECT_EQ(refused([](ratcast_stream_t& s) { s.kind = RATCAST_MEDIA_AUDIO; }),
              RATCAST_ERR_INVALID_ARG)
        << "AV1 is no sound";
    EXPECT_EQ(refused([](ratcast_stream_t& s) { s.kind = static_cast<ratcast_media_kind_t>(257); }),
              RATCAST_ERR_INVALID_ARG);
    EXPECT_EQ(refused([](ratcast_stream_t& s) {
                  s.label = "a label well over sixty-four bytes long, which no peer is sent at all";
              }),
              RATCAST_ERR_INVALID_ARG);
    EXPECT_EQ(refused([](ratcast_stream_t& s) { s.codec_config_size = 4; }),
              RATCAST_ERR_INVALID_ARG)
        << "a size with no bytes";

    ratcast_stream_t mic{};
    mic.id = 2;
    mic.codec = RATCAST_CODEC_OPUS;
    mic.lifetime_us = 100'000;
    mic.audio.sample_rate = 48000;
    mic.audio.channels = 256;
    EXPECT_EQ(ratcast_add_stream(sender_, &mic, nullptr), RATCAST_ERR_INVALID_ARG);
    mic.audio.channels = 2;
    EXPECT_EQ(ratcast_add_stream(sender_, &mic, nullptr), RATCAST_OK);
    EXPECT_EQ(ratcast_add_stream(sender_, &mic, nullptr), RATCAST_ERR_EXISTS);

    ratcast_stream_t other = screen();
    other.id = 9;
    other.codec = static_cast<ratcast_codec_t>(200);
    other.kind = RATCAST_MEDIA_VIDEO;
    EXPECT_EQ(ratcast_add_stream(sender_, &other, nullptr), RATCAST_OK) << "carried through";

    const ratcast_stream_t s = screen();
    const ratcast_bitrate_t backwards{
        .min_bps = 2'000'000, .start_bps = 1'000'000, .max_bps = 3'000'000};
    EXPECT_EQ(ratcast_add_stream(sender_, &s, &backwards), RATCAST_ERR_INVALID_ARG);
    EXPECT_EQ(ratcast_update_stream(sender_, &s), RATCAST_ERR_NOT_FOUND);
    EXPECT_EQ(ratcast_add_stream(sender_, nullptr, nullptr), RATCAST_ERR_INVALID_ARG);
}

TEST_F(CApiTest, AFrameThatCannotBeSentIsRefused) {
    const ratcast_stream_t s = screen();
    const ratcast_bitrate_t bitrate{
        .min_bps = 500'000, .start_bps = 1'000'000, .max_bps = 8'000'000};
    ASSERT_EQ(ratcast_add_stream(sender_, &s, &bitrate), RATCAST_OK);

    const std::vector<uint8_t> bytes = payload(0, 100);
    ratcast_frame_t f = frame_of(s.id, 5, bytes);
    EXPECT_EQ(ratcast_send_frame(sender_, &f), RATCAST_OK) << "no subscriber is no failure";
    EXPECT_EQ(ratcast_send_frame(sender_, &f), RATCAST_ERR_INVALID_ARG) << "the same number";

    ratcast_frame_t g = frame_of(s.id, 6, bytes);
    g.keyframe = 0;
    g.reference_count = 1;
    g.references[0] = 6;  // itself
    EXPECT_EQ(ratcast_send_frame(sender_, &g), RATCAST_ERR_INVALID_ARG);
    g.reference_count = RATCAST_MAX_REFERENCES + 1;
    EXPECT_EQ(ratcast_send_frame(sender_, &g), RATCAST_ERR_INVALID_ARG);
    g.reference_count = 0;
    g.temporal_layer = RATCAST_MAX_TEMPORAL_LAYER + 1;
    EXPECT_EQ(ratcast_send_frame(sender_, &g), RATCAST_ERR_INVALID_ARG);
    g.temporal_layer = 0;
    g.capture_time_us = -1;
    EXPECT_EQ(ratcast_send_frame(sender_, &g), RATCAST_ERR_INVALID_ARG);
    g.capture_time_us = ratcast_now_us();
    g.size = RATS_LIVE_MAX_PAYLOAD + 1;
    EXPECT_EQ(ratcast_send_frame(sender_, &g), RATCAST_ERR_INVALID_ARG);
    g.size = bytes.size();
    g.stream = 77;
    EXPECT_EQ(ratcast_send_frame(sender_, &g), RATCAST_ERR_NOT_FOUND);

    ratcast_outgoing_stats_t stats{};
    ASSERT_EQ(ratcast_outgoing_stats(sender_, s.id, &stats), RATCAST_OK);
    EXPECT_EQ(stats.frames, 1u);
    EXPECT_EQ(stats.bitrate_bps, bitrate.start_bps);
    EXPECT_EQ(ratcast_outgoing_stats(sender_, 77, &stats), RATCAST_ERR_NOT_FOUND);

    ratcast_remove_stream(sender_, s.id);
    EXPECT_EQ(ratcast_outgoing_stats(sender_, s.id, &stats), RATCAST_ERR_NOT_FOUND);
}

TEST_F(CApiTest, VideoReachesASubscriberWhole) {
    ratcast_stream_t s = screen();
    const uint8_t config[] = {0x81, 0x00, 0x0c, 0x00};
    s.codec_config = config;
    s.codec_config_size = sizeof config;
    ASSERT_EQ(ratcast_add_stream(sender_, &s, nullptr), RATCAST_OK);
    ASSERT_NO_FATAL_FAILURE(connect());

    ASSERT_TRUE(wait_for([&] { return viewer_seen_.announced_count() == 1; }));
    {
        std::lock_guard lock(viewer_seen_.mutex);
        EXPECT_EQ(viewer_seen_.announced_peer, sender_id_);
        EXPECT_EQ(viewer_seen_.label, "screen");
        EXPECT_EQ(viewer_seen_.codec_config, std::vector<uint8_t>(config, config + sizeof config));
        EXPECT_EQ(viewer_seen_.stream.kind, RATCAST_MEDIA_VIDEO);
        EXPECT_EQ(viewer_seen_.stream.codec, RATCAST_CODEC_AV1);
        EXPECT_EQ(viewer_seen_.stream.video.width, 1920u);
        EXPECT_EQ(viewer_seen_.stream.video.frame_rate_den, 1u);
        EXPECT_EQ(viewer_seen_.stream.lifetime_us, 500'000u);
    }

    EXPECT_EQ(ratcast_subscribe(viewer_, "not a peer id", s.id, 0), RATCAST_ERR_INVALID_ARG);
    EXPECT_EQ(ratcast_subscribe(viewer_, sender_id_.c_str(), 42, 0), RATCAST_ERR_NOT_FOUND);
    ASSERT_EQ(ratcast_subscribe(viewer_, sender_id_.c_str(), s.id, 0), RATCAST_OK);
    ASSERT_TRUE(wait_for([&] { return sender_seen_.keyframes_needed > 0; }))
        << "a subscriber starts at a keyframe";

    // A keyframe, then frames that each reference the one before, a base and
    // an enhancement layer taking turns.
    constexpr uint64_t kFrames = 20;
    for (uint64_t n = 0; n < kFrames; ++n) {
        const std::vector<uint8_t> bytes = payload(n, 3000);
        ratcast_frame_t f = frame_of(s.id, n, bytes);
        f.keyframe = n == 0;
        f.temporal_layer = static_cast<uint8_t>(n % 2);
        if (n > 0) {
            f.reference_count = 1;
            f.references[0] = n - 1;
        }
        ASSERT_EQ(ratcast_send_frame(sender_, &f), RATCAST_OK);
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(wait_for([&] { return viewer_seen_.frame_count() == kFrames; }));
    {
        std::lock_guard lock(viewer_seen_.mutex);
        for (uint64_t n = 0; n < kFrames; ++n) {
            const ratcast_frame_t& f = viewer_seen_.frames[n];
            EXPECT_EQ(f.number, n);
            EXPECT_EQ(f.stream, s.id);
            EXPECT_EQ(f.keyframe, n == 0 ? 1 : 0);
            EXPECT_EQ(f.temporal_layer, n % 2);
            EXPECT_EQ(f.reference_count, n == 0 ? 0 : 1);
            if (n > 0) {
                EXPECT_EQ(f.references[0], n - 1);
            }
            EXPECT_EQ(viewer_seen_.payloads[n], payload(n, 3000)) << "frame " << n;
        }
        EXPECT_LT(viewer_seen_.frames[0].capture_time_us,
                  viewer_seen_.frames.back().capture_time_us);
    }

    ratcast_incoming_stats_t in{};
    ASSERT_EQ(ratcast_incoming_stats(viewer_, sender_id_.c_str(), s.id, &in), RATCAST_OK);
    EXPECT_EQ(in.subscribed, 1);
    EXPECT_EQ(in.frames_delivered, kFrames);
    EXPECT_EQ(in.played, 0u) << "not subscribed with a delay";
    ratcast_outgoing_stats_t out{};
    ASSERT_EQ(ratcast_outgoing_stats(sender_, s.id, &out), RATCAST_OK);
    EXPECT_EQ(out.subscribers, 1u);
    EXPECT_EQ(out.messages, kFrames);
    EXPECT_EQ(ratcast_request_keyframe(viewer_, sender_id_.c_str(), s.id), RATCAST_OK);
    EXPECT_EQ(ratcast_request_keyframe(viewer_, sender_id_.c_str(), 42), RATCAST_ERR_NOT_FOUND);

    ASSERT_EQ(ratcast_unsubscribe(viewer_, sender_id_.c_str(), s.id), RATCAST_OK);
    ASSERT_TRUE(wait_for([&] {
        return ratcast_outgoing_stats(sender_, s.id, &out) == RATCAST_OK && out.subscribers == 0;
    }));
    ratcast_remove_stream(sender_, s.id);
    ASSERT_TRUE(wait_for([&] { return viewer_seen_.withdrawn_count() == 1; }));
    EXPECT_EQ(ratcast_rejected_messages(viewer_), 0u);
}

TEST_F(CApiTest, AFeedSubscribedWithADelayPlaysOutInOrderThenEnds) {
    const ratcast_stream_t s = feed();
    ASSERT_EQ(ratcast_add_stream(sender_, &s, nullptr), RATCAST_OK);
    ASSERT_NO_FATAL_FAILURE(connect());
    ASSERT_TRUE(wait_for([&] { return viewer_seen_.announced_count() == 1; }));
    {
        std::lock_guard lock(viewer_seen_.mutex);
        EXPECT_EQ(viewer_seen_.stream.kind, RATCAST_MEDIA_DATA);
    }
    constexpr uint32_t kDelay = 50'000;
    ASSERT_EQ(ratcast_subscribe(viewer_, sender_id_.c_str(), s.id, kDelay), RATCAST_OK);
    ratcast_outgoing_stats_t out{};
    ASSERT_TRUE(wait_for([&] {
        return ratcast_outgoing_stats(sender_, s.id, &out) == RATCAST_OK && out.subscribers == 1;
    }));

    constexpr uint64_t kFrames = 50;
    std::vector<int64_t> sent_at;
    for (uint64_t n = 0; n < kFrames; ++n) {
        const std::vector<uint8_t> bytes = payload(n, RATCAST_DATA_FRAME_SIZE);
        const ratcast_frame_t f = frame_of(s.id, n, bytes);
        sent_at.push_back(f.capture_time_us);
        ASSERT_EQ(ratcast_send_frame(sender_, &f), RATCAST_OK);
        std::this_thread::sleep_for(2ms);
    }
    // Withdrawn at once, but told only once what was held is played.
    ratcast_remove_stream(sender_, s.id);
    ASSERT_TRUE(wait_for([&] { return viewer_seen_.withdrawn_count() == 1; }));
    {
        std::lock_guard lock(viewer_seen_.mutex);
        EXPECT_EQ(viewer_seen_.frames_when_withdrawn, kFrames);
        ASSERT_EQ(viewer_seen_.frames.size(), kFrames);
        for (uint64_t n = 0; n < kFrames; ++n) {
            EXPECT_EQ(viewer_seen_.frames[n].number, n);
            EXPECT_EQ(viewer_seen_.payloads[n], payload(n, RATCAST_DATA_FRAME_SIZE)) << n;
        }
    }
    EXPECT_EQ(ratcast_incoming_stats(viewer_, sender_id_.c_str(), s.id, nullptr),
              RATCAST_ERR_INVALID_ARG);
}

TEST_F(CApiTest, AFeedPlaysAtItsDelayAndStopsWhenUnsubscribed) {
    const ratcast_stream_t s = feed();
    ASSERT_EQ(ratcast_add_stream(sender_, &s, nullptr), RATCAST_OK);
    ASSERT_NO_FATAL_FAILURE(connect());
    ASSERT_TRUE(wait_for([&] { return viewer_seen_.announced_count() == 1; }));
    constexpr uint32_t kDelay = 80'000;
    ASSERT_EQ(ratcast_subscribe(viewer_, sender_id_.c_str(), s.id, kDelay), RATCAST_OK);
    ratcast_outgoing_stats_t out{};
    ASSERT_TRUE(wait_for([&] {
        return ratcast_outgoing_stats(sender_, s.id, &out) == RATCAST_OK && out.subscribers == 1;
    }));

    const std::vector<uint8_t> bytes = payload(0, 188);
    const ratcast_frame_t f = frame_of(s.id, 0, bytes);
    ASSERT_EQ(ratcast_send_frame(sender_, &f), RATCAST_OK);
    ASSERT_TRUE(wait_for([&] { return viewer_seen_.frame_count() == 1; }));
    // The same clock on both ends here: played its delay past being sent.
    EXPECT_GE(ratcast_now_us() - f.capture_time_us, int64_t{kDelay});

    ratcast_incoming_stats_t in{};
    ASSERT_EQ(ratcast_incoming_stats(viewer_, sender_id_.c_str(), s.id, &in), RATCAST_OK);
    EXPECT_EQ(in.played, 1u);

    // Unsubscribed with a frame held: it is never handed on.
    const ratcast_frame_t g = frame_of(s.id, 1, bytes);
    ASSERT_EQ(ratcast_send_frame(sender_, &g), RATCAST_OK);
    ASSERT_TRUE(wait_for([&] {
        return ratcast_incoming_stats(viewer_, sender_id_.c_str(), s.id, &in) == RATCAST_OK &&
               in.frames_received == 2;
    }));
    ASSERT_EQ(ratcast_unsubscribe(viewer_, sender_id_.c_str(), s.id), RATCAST_OK);
    std::this_thread::sleep_for(std::chrono::microseconds(kDelay) + 20ms);
    EXPECT_EQ(viewer_seen_.frame_count(), 1u);
}

TEST_F(CApiTest, AFrameHandlerMayUnsubscribeItsOwnStream) {
    const ratcast_stream_t s = feed();
    ASSERT_EQ(ratcast_add_stream(sender_, &s, nullptr), RATCAST_OK);
    ASSERT_NO_FATAL_FAILURE(connect());
    ASSERT_TRUE(wait_for([&] { return viewer_seen_.announced_count() == 1; }));
    viewer_seen_.media = viewer_;
    viewer_seen_.unsubscribe_at = 4;
    ASSERT_EQ(ratcast_subscribe(viewer_, sender_id_.c_str(), s.id, 20'000), RATCAST_OK);
    ratcast_outgoing_stats_t out{};
    ASSERT_TRUE(wait_for([&] {
        return ratcast_outgoing_stats(sender_, s.id, &out) == RATCAST_OK && out.subscribers == 1;
    }));

    // Sent at once, so that they are played together: the handler unsubscribes
    // amid them, on the stream's own thread.
    for (uint64_t n = 0; n < 10; ++n) {
        const std::vector<uint8_t> bytes = payload(n, 188);
        const ratcast_frame_t f = frame_of(s.id, n, bytes);
        ASSERT_EQ(ratcast_send_frame(sender_, &f), RATCAST_OK);
    }
    ASSERT_TRUE(wait_for([&] { return viewer_seen_.frame_count() >= 5; }));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(viewer_seen_.frame_count(), 5u) << "nothing after the handler unsubscribed";

    // Subscribed again from here, it plays again.
    viewer_seen_.unsubscribe_at = UINT64_MAX;
    ASSERT_EQ(ratcast_subscribe(viewer_, sender_id_.c_str(), s.id, 20'000), RATCAST_OK);
    ASSERT_TRUE(wait_for([&] {
        return ratcast_outgoing_stats(sender_, s.id, &out) == RATCAST_OK && out.subscribers == 1;
    }));
    const std::vector<uint8_t> bytes = payload(10, 188);
    const ratcast_frame_t f = frame_of(s.id, 10, bytes);
    ASSERT_EQ(ratcast_send_frame(sender_, &f), RATCAST_OK);
    EXPECT_TRUE(wait_for([&] { return viewer_seen_.frame_count() == 6; }));
}

TEST(CApiNamesTest, ErrorNamesAndVersion) {
    EXPECT_STREQ(RATCAST_PROTOCOL, "ratcast/0") << "changing it parts ratcast from its own tools";
    EXPECT_STREQ(ratcast_error_str(RATCAST_ERR_NOT_FOUND), "RATCAST_ERR_NOT_FOUND");
    EXPECT_STREQ(ratcast_error_str(static_cast<ratcast_error_t>(99)), "RATCAST_ERR_UNKNOWN");
    EXPECT_STRNE(ratcast_version_string(), "");
    EXPECT_EQ(ratcast_abi() >> 16, static_cast<uint32_t>(RATCAST_VERSION_MAJOR));
    const int64_t before = ratcast_now_us();
    std::this_thread::sleep_for(1ms);
    EXPECT_GT(ratcast_now_us(), before);
}

}  // namespace
