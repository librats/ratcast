#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "ratcast/server/server.h"
#include "ratcast/session/media_service.h"

#include "librats/node/node.h"
#include "librats/subsystems/hole_punch.h"
#include "librats/subsystems/relay.h"
#include "librats/subsystems/rendezvous.h"

namespace ratcast {
namespace {

using namespace std::chrono_literals;

constexpr const char* kProtocol = "ratcast-test/server";

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

std::unique_ptr<Server> start_server() {
    ServerConfig config;
    config.port = 0;
    config.bind_address = "127.0.0.1";
    config.protocol = kProtocol;
    auto server = Server::start(config);
    return server ? std::move(*server) : nullptr;
}

/// A call's node as far as the server is concerned: met under `key` through
/// the server at `port` (none: not met), punching and relayed as a call is.
std::unique_ptr<librats::Node> call_node(const std::string& key, uint16_t port) {
    librats::NodeConfig config;
    config.bind_address = "127.0.0.1";
    config.protocol = kProtocol;
    auto node = std::make_unique<librats::Node>(config);
    if (!key.empty()) {
        librats::Rendezvous::Config meeting;
        meeting.keys = {key};
        meeting.servers = {librats::HostEndpoint("127.0.0.1", port)};
        node->add_subsystem(std::make_unique<librats::Rendezvous>(meeting));
    }
    node->add_subsystem(std::make_unique<librats::HolePunch>());
    node->add_subsystem(std::make_unique<librats::Relay>());
    return node;
}

std::optional<librats::TransportKind> transport_to(librats::Node& node, const PeerId& peer) {
    for (const librats::PeerInfo& info : node.peers())
        if (info.id == peer) return info.transport;
    return std::nullopt;
}

TEST(Server, CallsOfOneRoomMeetThroughIt) {
    const auto server = start_server();
    ASSERT_TRUE(server);
    auto a = call_node("ratcast/stream/room", server->port());
    auto b = call_node("ratcast/stream/room", server->port());
    ASSERT_TRUE(a->start() && b->start());

    EXPECT_TRUE(wait_for([&] { return a->peer(b->local_id()) && b->peer(a->local_id()); }));
    EXPECT_EQ(transport_to(*a, b->local_id()), librats::TransportKind::Udp) << "not direct";
    EXPECT_TRUE(wait_for([&] {
        const ServerStats stats = server->stats();
        return stats.peers == 2 && stats.registrations == 2;
    }));
    EXPECT_EQ(server->stats().relayed, 0u);
    b->stop();
    a->stop();
}

// Two it holds that cannot reach each other otherwise are carried, the
// connection end to end.
TEST(Server, CarriesAConnectionBetweenTwoItHolds) {
    const auto server = start_server();
    ASSERT_TRUE(server);
    auto a = call_node("", 0);
    auto b = call_node("", 0);
    ASSERT_TRUE(a->start() && b->start());
    a->connect("127.0.0.1", server->port());
    b->connect("127.0.0.1", server->port());
    ASSERT_TRUE(wait_for([&] { return server->stats().peers == 2; }));

    auto* relay = a->services().get<librats::RelayService>();
    ASSERT_NE(relay, nullptr);
    ASSERT_TRUE(wait_for([&] { return relay->serves(server->id()); }));
    ASSERT_TRUE(relay->connect_via_relay(b->local_id()));
    EXPECT_TRUE(wait_for([&] { return a->peer(b->local_id()).has_value(); }));
    // Carried at first: a punch over the circuit may replace it since.
    EXPECT_TRUE(wait_for([&] { return server->stats().relayed_bytes > 0; }));
    b->stop();
    a->stop();
}

/// A call's node with a media session, relayed by the server where asked.
struct MediaNode {
    explicit MediaNode(SessionHandlers handlers = {}) {
        librats::NodeConfig config;
        config.bind_address = "127.0.0.1";
        config.protocol = kProtocol;
        node = std::make_unique<librats::Node>(config);
        media = node->add_subsystem(std::make_unique<MediaService>(std::move(handlers)));
        // No HolePunch: a connection the server carries stays so.
        relay = node->add_subsystem(std::make_unique<librats::Relay>());
    }
    ~MediaNode() { node->stop(); }

    std::unique_ptr<librats::Node> node;
    MediaService* media = nullptr;
    librats::Relay* relay = nullptr;
};

// Viewers whose connections the server carries get the share through its
// forwarder: one copy from the sender for all of them.
TEST(Server, ForwardsAShareToTheViewersItCarries) {
    const auto server = start_server();
    ASSERT_TRUE(server);
    std::atomic<int> pictures{0};
    MediaNode sender;
    MediaNode a({.frame_received = [&](const PeerId&, EncodedFrame) { ++pictures; }});
    MediaNode b({.frame_received = [&](const PeerId&, EncodedFrame) { ++pictures; }});
    for (MediaNode* n : {&sender, &a, &b}) {
        ASSERT_TRUE(n->node->start());
        n->node->connect("127.0.0.1", server->port());
    }
    ASSERT_TRUE(wait_for([&] { return server->stats().peers == 3; }));
    const PeerId& from = sender.node->local_id();
    for (MediaNode* viewer : {&a, &b}) {
        ASSERT_TRUE(wait_for([&] { return viewer->relay->serves(server->id()); }));
        ASSERT_TRUE(viewer->relay->connect_via_relay(from));
        ASSERT_TRUE(wait_for([&] { return transport_to(*viewer->node, from).has_value(); }));
        ASSERT_EQ(transport_to(*viewer->node, from), librats::TransportKind::Relay);
    }

    StreamDescription screen;
    screen.id = 1;
    screen.codec = Codec::AV1;
    screen.format = VideoFormat{640, 360, 30, 1};
    screen.lifetime = std::chrono::milliseconds(200);
    screen.label = "screen";
    MediaSession& sending = sender.media->session();
    ASSERT_TRUE(sending.add_stream(screen, BitrateLimits{250'000, 1'000'000, 2'000'000}));
    for (MediaNode* viewer : {&a, &b}) {
        MediaSession& session = viewer->media->session();
        ASSERT_TRUE(wait_for([&] { return !session.offered().empty(); }));
        ASSERT_TRUE(session.subscribe(from, 1));
    }
    ASSERT_TRUE(wait_for([&] { return sending.outgoing_stats(1)->relayed == 2; }));

    for (FrameNumber n = 0; pictures < 20 && n < 300; ++n) {
        FrameInfo info;
        info.number = n;
        info.keyframe = n % 30 == 0;
        if (!info.keyframe) info.references = {n - 1};
        info.capture_time = SteadyClock::instance().now();
        MutableBuffer payload = allocate_frame_payload(2000);
        payload.append(2000);
        ASSERT_TRUE(sending.send_frame(1, info, std::move(payload)));
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    EXPECT_GE(pictures.load(), 20);
    // Sealed end to end: the viewers opened what the server could not.
    for (MediaNode* viewer : {&a, &b}) {
        const auto stream = viewer->media->session().incoming_stats(from, 1);
        EXPECT_GT(stream->opened, 0u);
        EXPECT_EQ(stream->unopened, 0u);
    }
    const OutgoingStreamStats sent = *sending.outgoing_stats(1);
    EXPECT_EQ(sent.subscribers, 2u);
    // One copy a frame, to the server.
    EXPECT_LE(sent.messages, sent.frames);
    const ServerStats stats = server->stats();
    EXPECT_EQ(stats.forwarded, 1u);
    EXPECT_EQ(stats.forwarded_to, 2u);
    EXPECT_GT(stats.forwarded_bytes, 0u);
}

TEST(Server, APortTakenIsUnavailable) {
    const auto first = start_server();
    ASSERT_TRUE(first);
    ServerConfig config;
    config.port = first->port();
    config.bind_address = "127.0.0.1";
    config.protocol = kProtocol;
    const auto second = Server::start(config);
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error(), Errc::Unavailable);
}

}  // namespace
}  // namespace ratcast
