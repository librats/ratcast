#pragma once

/**
 * @file proxy.h
 * @brief A UDP proxy whose two directions are emulated links: what a dialer
 * sends it goes on to the target over one Link, what the target answers
 * comes back over the other.
 *
 * Put between a viewer and the sender it dials, it is a path as bench/netem.py
 * shapes one with tc netem, without root or Linux: the viewer dials the
 * proxy, which forwards to the sender. Each dialer gets a socket of its own
 * toward the target, so the target tells them apart as it would real peers.
 *
 * One thread runs it all: it reads what comes, hands each datagram to its
 * link, and sends it on when the link says it leaves — to the millisecond the
 * poller waits in, so a little later than netem's timers.
 */

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <vector>

#include "netsim/link.h"
#include "ratcast/core/time.h"

#include "librats/core/address.h"
#include "librats/core/io_poller.h"
#include "librats/core/socket.h"

namespace ratcast::netsim {

struct ProxyConfig {
    /// Where dialers send to. Port 0: any, see Proxy::port().
    librats::Address listen = {};
    /// Where it all goes on to.
    librats::Address target = {};
    /// From the target to its dialers: a sender's stream to its viewers.
    LinkConfig forward = {};
    /// From the dialers to the target.
    LinkConfig backward = {};
    /// The forward link changed `at` after the first datagram came: a path
    /// that narrows and widens again.
    struct Change {
        Duration at{};
        LinkConfig forward = {};
    };
    std::vector<Change> changes = {};
    uint64_t seed = 1;
};

class Proxy {
public:
    struct Stats {
        Link::Stats forward;
        Link::Stats backward;
        uint64_t forward_delivered = 0;  ///< datagrams that left the forward link
        uint64_t forward_bytes = 0;      ///< their payload
        uint64_t backward_delivered = 0;
        uint64_t backward_bytes = 0;
        uint64_t send_failures = 0;  ///< sends the system refused: lost as well
        size_t dialers = 0;
    };

    /// Bound and ready to run, or null with `error` saying why.
    static std::unique_ptr<Proxy> open(ProxyConfig config, std::string* error = nullptr);
    ~Proxy();
    Proxy(const Proxy&) = delete;
    Proxy& operator=(const Proxy&) = delete;

    /// The port dialers send to.
    uint16_t port() const noexcept { return port_; }

    /// Run until stop(), on the calling thread.
    void run();
    /// Make run() return, from any thread.
    void stop() noexcept { stopping_.store(true); }

    Stats stats() const;

private:
    struct Departure {
        Timestamp at;
        uint64_t order;  ///< ties leave as they came
        librats::socket_t socket;
        librats::Address to;
        std::vector<uint8_t> bytes;
        bool forward;

        bool operator>(const Departure& other) const noexcept {
            return at != other.at ? at > other.at : order > other.order;
        }
    };

    explicit Proxy(ProxyConfig config);

    void read(librats::socket_t socket, std::vector<uint8_t>& buffer, Timestamp now);
    /// The socket toward the target for `dialer`, opened on its first datagram.
    librats::socket_t upstream(const librats::Address& dialer);
    void schedule(Link& link, bool forward, librats::socket_t socket, const librats::Address& to,
                  std::span<const uint8_t> bytes, Timestamp now);
    void send_due(Timestamp now);
    void apply_changes(Timestamp now);

    const ProxyConfig config_;
    std::unique_ptr<librats::IOPoller> poller_;
    librats::socket_t listen_ = RATS_INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};

    // The thread's own.
    std::map<librats::Address, librats::socket_t> upstream_;   ///< by dialer
    std::map<librats::socket_t, librats::Address> dialer_of_;  ///< by upstream socket
    std::priority_queue<Departure, std::vector<Departure>, std::greater<>> departures_;
    uint64_t order_ = 0;
    std::optional<Timestamp> first_;  ///< the first datagram
    size_t changes_done_ = 0;

    mutable std::mutex mutex_;  ///< the links and the stats, for stats()
    Link forward_;
    Link backward_;
    Stats stats_;
};

}  // namespace ratcast::netsim
