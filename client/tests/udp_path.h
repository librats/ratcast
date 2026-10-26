#pragma once

// A path between two calls on loopback that can go dead: a UDP forwarder.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "librats/core/address.h"
#include "librats/core/socket.h"

namespace ratcast::test {

/// What comes to its port goes on to `to` from a socket of its own, and what
/// comes back there goes to whoever last sent to its port. While `dead`,
/// nothing passes either way: the network gone, with neither end told.
class UdpPath {
public:
    explicit UdpPath(uint16_t to)
        : to_(*librats::Address::parse("127.0.0.1:" + std::to_string(to))),
          front_(open()),
          back_(open()),
          thread_([this] { run(); }) {}
    ~UdpPath() {
        running_ = false;
        thread_.join();
        librats::close_socket(front_);
        librats::close_socket(back_);
    }
    UdpPath(const UdpPath&) = delete;
    UdpPath& operator=(const UdpPath&) = delete;

    uint16_t port() const { return static_cast<uint16_t>(librats::get_bound_port(front_)); }

    std::atomic<bool> dead{false};

private:
    static librats::socket_t open() {
        return librats::create_udp_socket(0, "127.0.0.1", librats::AddressFamily::IPv4,
                                          librats::UdpPortMode::Exclusive);
    }

    void run() {
        using librats::AddressFamily;
        librats::Address client;
        while (running_) {
            bool moved = false;
            librats::Address from;
            const std::vector<uint8_t> out = librats::receive_udp_data(front_, 65536, from, 0);
            if (!out.empty()) {
                moved = true;
                client = from;
                if (!dead) librats::send_udp_data(back_, out, to_, AddressFamily::IPv4);
            }
            const std::vector<uint8_t> in = librats::receive_udp_data(back_, 65536, from, 0);
            if (!in.empty()) {
                moved = true;
                if (!dead && client.port != 0) {
                    librats::send_udp_data(front_, in, client, AddressFamily::IPv4);
                }
            }
            if (!moved) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    const librats::Address to_;
    const librats::socket_t front_;
    const librats::socket_t back_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

}  // namespace ratcast::test
