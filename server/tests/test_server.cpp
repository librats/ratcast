#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include "ratcast/server/server.h"

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
