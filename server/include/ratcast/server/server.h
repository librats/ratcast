#pragma once

/**
 * @file server.h
 * @brief A public node that calls behind NATs meet through and, where no path
 * between two of them can be punched, are carried by.
 *
 * The server is no member of any call. A call that knows it (CallConfig::
 * servers, or a member's peer exchange naming it) registers its room's key
 * with it (librats' Rendezvous: a hash of the key, so no link is learnt); the
 * server tells each member of the others, at the endpoints their NATs map
 * them to, all at the same moment, and their dials cross. Where those do not
 * get through, it carries the punch's rendezvous (HolePunch); where no punch
 * can (a NAT that maps the socket anew for each destination), it carries the
 * connection itself (Relay: the packets of an end-to-end encrypted
 * connection, forwarded as they come — it reads nothing). Calls that never
 * reach it work as before: it only makes more of them connect.
 *
 * And it forwards, an SFU (SessionConfig::forwarder): a sharer whose uplink
 * is short of a copy for each viewer hands it viewers before any member, and
 * sends it one copy, which it passes on to them all; so too every viewer
 * whose connection it carries anyway. It watches nothing, decodes nothing,
 * keeps no frame.
 *
 * Threads: every method may be called from any thread.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/ratcast.h"
#include "ratcast/session/media_network.h"

namespace librats {
class HolePunch;
class Node;
class Relay;
class Rendezvous;
}  // namespace librats

namespace ratcast {

class MediaService;

struct ServerConfig {
    /// The port calls dial, UDP and TCP; 0: any.
    uint16_t port = 7700;
    /// The interface to listen on: "" for every one, IPv6 and IPv4.
    std::string bind_address = {};
    /// Where the server keeps its identity — its peer id — across runs; "": a
    /// new identity each run.
    std::string data_dir = {};
    std::string protocol = RATCAST_PROTOCOL;
    /// What one carried connection may move each way, in bits per second: a
    /// screen's share and its sound with room to spare.
    uint64_t relay_rate_bps = 20'000'000;
    /// Connections carried at once, and opened by one peer: a member behind a
    /// NAT no punch gets through needs one to each other member.
    size_t max_relayed = 64;
    size_t max_relayed_per_peer = 8;
    /// Peers connected at once.
    size_t max_peers = 1024;
};

struct ServerStats {
    size_t peers = 0;          ///< connected now
    size_t registrations = 0;  ///< of rooms' keys, every peer's
    size_t relayed = 0;        ///< connections carried now
    uint64_t relayed_bytes = 0;
    uint64_t punches = 0;          ///< rendezvous messages carried for punches
    size_t forwarded = 0;          ///< streams forwarded now
    size_t forwarded_to = 0;       ///< viewers they are passed on to
    uint64_t forwarded_bytes = 0;  ///< passed on so far, of the streams forwarded now
};

class Server {
public:
    /// Fails with Unavailable when the node cannot start (the port is taken).
    static Result<std::unique_ptr<Server>> start(ServerConfig config);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    const PeerId& id() const noexcept;
    uint16_t port() const noexcept;
    ServerStats stats() const;

private:
    explicit Server(const ServerConfig& config);

    std::unique_ptr<librats::Node> node_;
    MediaService* media_ = nullptr;  ///< the node's
    librats::Rendezvous* rendezvous_ = nullptr;
    librats::HolePunch* punch_ = nullptr;
    librats::Relay* relay_ = nullptr;
};

}  // namespace ratcast
