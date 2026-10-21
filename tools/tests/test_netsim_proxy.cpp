// netsim's Proxy on loopback: datagrams there and back across its links, a
// socket of its own per dialer, and — the scenario it exists for — ratcast
// repairing a lossy path it emulates, between two real librats nodes.

#include "netsim/proxy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ratcast/session/media_service.h"

#include "librats/node/node.h"

using namespace ratcast;
using namespace ratcast::netsim;
using namespace std::chrono_literals;

namespace {

const librats::Address kLoopback{"127.0.0.1", 0};

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

LinkConfig link(std::string_view options) { return parse_link(options).value(); }

/// A UDP socket on loopback, polled without blocking.
class Socket {
public:
    Socket() {
        librats::init_socket_library();
        socket_ = librats::create_udp_socket(0, "127.0.0.1", librats::AddressFamily::IPv4);
        librats::set_socket_nonblocking(socket_);
    }
    ~Socket() { librats::close_socket(socket_); }

    librats::Address address() const {
        return {"127.0.0.1", static_cast<uint16_t>(librats::get_bound_port(socket_))};
    }

    void send(const std::vector<uint8_t>& bytes, const librats::Address& to) {
        librats::send_udp_to(socket_, bytes.data(), bytes.size(), to, librats::AddressFamily::IPv4);
    }

    /// The next datagram and who sent it, if one comes within `timeout`.
    std::optional<std::pair<std::vector<uint8_t>, librats::Address>> receive(
        std::chrono::milliseconds timeout = 2s) {
        std::vector<uint8_t> buffer(2048);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            librats::Address from;
            const std::ptrdiff_t size =
                librats::recv_udp_from(socket_, buffer.data(), buffer.size(), from);
            if (size >= 0) {
                buffer.resize(static_cast<size_t>(size));
                return std::pair{buffer, from};
            }
            std::this_thread::sleep_for(200us);
        }
        return std::nullopt;
    }

private:
    librats::socket_t socket_;
};

/// A proxy running on a thread of its own.
class Running {
public:
    explicit Running(ProxyConfig config) {
        config.listen = kLoopback;
        std::string error;
        proxy_ = Proxy::open(std::move(config), &error);
        EXPECT_TRUE(proxy_) << error;
        if (proxy_) thread_ = std::thread([this] { proxy_->run(); });
    }
    ~Running() {
        if (!proxy_) return;
        proxy_->stop();
        thread_.join();
    }

    librats::Address address() const { return {"127.0.0.1", proxy_->port()}; }
    Proxy& operator*() { return *proxy_; }
    Proxy* operator->() { return proxy_.get(); }

private:
    std::unique_ptr<Proxy> proxy_;
    std::thread thread_;
};

TEST(NetsimProxyTest, DatagramsCrossBothLinksAndBack) {
    Socket target;
    Socket dialer;
    Running proxy({.target = target.address(),
                   .forward = link("delay 30ms"),
                   .backward = link("delay 10ms")});

    std::vector<std::chrono::steady_clock::duration> took;
    for (uint8_t i = 0; i < 20; ++i) {
        const auto sent = std::chrono::steady_clock::now();
        dialer.send({i, 1, 2, 3}, proxy.address());
        const auto there = target.receive();
        ASSERT_TRUE(there);
        EXPECT_EQ(there->first, (std::vector<uint8_t>{i, 1, 2, 3}));
        target.send({i, 9}, there->second);  // the answer, to whoever asked
        const auto back = dialer.receive();
        ASSERT_TRUE(back);
        EXPECT_EQ(back->first, (std::vector<uint8_t>{i, 9}));
        EXPECT_EQ(back->second, proxy.address()) << "answered by the proxy";
        took.push_back(std::chrono::steady_clock::now() - sent);
        EXPECT_GE(took.back(), 40ms) << "never sooner than the path";
    }
    // As late as the host's timers wake: within a millisecond on most, up to
    // ten on a virtual machine that sleeps.
    std::ranges::nth_element(took, took.begin() + 10);
    EXPECT_LT(took[10], 55ms);
    const Proxy::Stats stats = proxy->stats();
    EXPECT_EQ(stats.backward_delivered, 20u);
    EXPECT_EQ(stats.forward_delivered, 20u);
    EXPECT_EQ(stats.forward_bytes, 40u);
    EXPECT_EQ(stats.dialers, 1u);
}

TEST(NetsimProxyTest, EachDialerHasASocketOfItsOwn) {
    Socket target;
    Socket first;
    Socket second;
    Running proxy({.target = target.address()});

    first.send({1}, proxy.address());
    second.send({2}, proxy.address());
    std::set<uint16_t> ports;
    for (int i = 0; i < 2; ++i) {
        const auto there = target.receive();
        ASSERT_TRUE(there);
        ports.insert(there->second.port);
        target.send(there->first, there->second);
    }
    EXPECT_EQ(ports.size(), 2u);
    EXPECT_EQ(first.receive()->first, std::vector<uint8_t>{1});
    EXPECT_EQ(second.receive()->first, std::vector<uint8_t>{2});
    EXPECT_EQ(proxy->stats().dialers, 2u);
}

TEST(NetsimProxyTest, ALinkThatLosesAllPassesNothing) {
    Socket target;
    Socket dialer;
    Running proxy({.target = target.address(), .backward = link("loss 100%")});
    for (uint8_t i = 0; i < 10; ++i) dialer.send({i}, proxy.address());
    EXPECT_FALSE(target.receive(100ms));
    ASSERT_TRUE(wait_for([&] { return proxy->stats().backward.packets == 10; }));
    EXPECT_EQ(proxy->stats().backward.lost, 10u);
}

TEST(NetsimProxyTest, TheForwardLinkChangesOnSchedule) {
    Socket target;
    Socket dialer;
    Running proxy({.target = target.address(),
                   .forward = link("delay 5ms"),
                   .changes = {{.at = 200ms, .forward = link("loss 100%")}}});
    dialer.send({0}, proxy.address());  // the first datagram: the schedule starts
    const auto there = target.receive();
    ASSERT_TRUE(there);
    target.send({1}, there->second);
    EXPECT_TRUE(dialer.receive());
    std::this_thread::sleep_for(250ms);
    target.send({2}, there->second);
    EXPECT_FALSE(dialer.receive(100ms)) << "lost once the path changed";
}

// ── ratcast across it ───────────────────────────────────────────────────────

librats::NodeConfig node_config(bool listen) {
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    librats::NodeConfig c;
    c.listen_port = 0;
    c.bind_address = "127.0.0.1";
    c.enable_listen = listen;
    c.enable_tcp = false;  // what is shaped is UDP
    c.enable_network_monitor = false;
    c.protocol = std::string("ratcast-test/") + test->test_suite_name() + "." + test->name();
    return c;
}

TEST(NetsimProxyTest, RatcastRepairsTheLossOfAnEmulatedPath) {
    // A feed across 50 ms of round trip and 5% loss: every frame arrives,
    // what was lost sent again within the frames' lifetime.
    librats::Node sender_node(node_config(true));
    librats::Node viewer_node(node_config(false));

    std::atomic<bool> announced{false};
    std::mutex mutex;
    std::set<FrameNumber> received;
    auto* sender = sender_node.add_subsystem(std::make_unique<MediaService>(SessionHandlers{}));
    auto* viewer = viewer_node.add_subsystem(std::make_unique<MediaService>(SessionHandlers{
        .stream_announced = [&](const PeerId&, const StreamDescription&) { announced = true; },
        .frame_received =
            [&](const PeerId&, EncodedFrame frame) {
                std::lock_guard lock(mutex);
                received.insert(frame.info.number);
            },
    }));

    StreamDescription feed;
    feed.id = 3;
    feed.codec = Codec::MpegTs;
    feed.format = DataFormat{};
    feed.lifetime = 400ms;
    feed.label = "ts";
    ASSERT_TRUE(sender->session().add_stream(feed));
    ASSERT_TRUE(sender_node.start());
    ASSERT_TRUE(viewer_node.start());

    Running proxy({.target = {"127.0.0.1", sender_node.listen_port()},
                   .forward = link("delay 25ms loss 5%"),
                   .backward = link("delay 25ms")});
    viewer_node.connect(proxy.address());
    ASSERT_TRUE(wait_for([&] { return announced.load(); }));
    const PeerId sender_id = sender_node.local_id();
    ASSERT_TRUE(viewer->session().subscribe(sender_id, feed.id));
    ASSERT_TRUE(
        wait_for([&] { return sender->session().outgoing_stats(feed.id)->subscribers == 1; }));

    constexpr FrameNumber kFrames = 300;
    for (FrameNumber n = 0; n < kFrames; ++n) {
        MutableBuffer payload = allocate_frame_payload(1128);
        payload.resize(1128);
        FrameInfo info;
        info.number = n;
        info.capture_time = SteadyClock::instance().now();
        info.keyframe = true;
        ASSERT_TRUE(sender->session().send_frame(feed.id, info, std::move(payload)));
        std::this_thread::sleep_for(3ms);
    }
    EXPECT_TRUE(wait_for(
        [&] {
            std::lock_guard lock(mutex);
            return received.size() == kFrames;
        },
        3s))
        << "received " << received.size() << " of " << kFrames;
    const Proxy::Stats stats = proxy->stats();
    EXPECT_GT(stats.forward.lost, 0u) << "the path lost some";
    EXPECT_EQ(stats.dialers, 1u);
}

}  // namespace
