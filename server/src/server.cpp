#include "ratcast/server/server.h"

#include "ratcast/session/media_service.h"

#include "librats/node/node.h"
#include "librats/subsystems/hole_punch.h"
#include "librats/subsystems/relay.h"
#include "librats/subsystems/rendezvous.h"

namespace ratcast {
namespace {

librats::NodeConfig node_config(const ServerConfig& config) {
    librats::NodeConfig node;
    node.listen_port = config.port;
    node.bind_address = config.bind_address;
    node.data_dir = config.data_dir;
    node.protocol = config.protocol;
    node.max_peers = config.max_peers;
    return node;
}

}  // namespace

Result<std::unique_ptr<Server>> Server::start(ServerConfig config) {
    std::unique_ptr<Server> server(new Server(config));
    if (!server->node_->start()) return fail(Errc::Unavailable);
    // A node finds another port near one taken; calls dial this one.
    if (config.port != 0 && server->port() != config.port) return fail(Errc::Unavailable);
    return server;
}

Server::Server(const ServerConfig& config)
    : node_(std::make_unique<librats::Node>(node_config(config))) {
    media_ = node_->add_subsystem(
        std::make_unique<MediaService>(SessionHandlers{}, SessionConfig{.forwarder = true}));
    librats::Rendezvous::Config meeting;
    meeting.serve = true;
    rendezvous_ = node_->add_subsystem(std::make_unique<librats::Rendezvous>(meeting));
    // It carries punches for others (on by default) and punches nobody itself.
    punch_ = node_->add_subsystem(std::make_unique<librats::HolePunch>());
    librats::Relay::Config relay;
    relay.enable_client = false;
    relay.accept_inbound = false;
    relay.serve = true;
    relay.max_circuits = config.max_relayed;
    relay.max_circuits_per_peer = config.max_relayed_per_peer;
    relay.max_circuit_rate_bps = config.relay_rate_bps;
    // A call lasts as long as it lasts: a carried one is not cut after an hour.
    relay.max_circuit_duration = std::chrono::seconds(0);
    relay_ = node_->add_subsystem(std::make_unique<librats::Relay>(relay));
}

Server::~Server() { node_->stop(); }

const PeerId& Server::id() const noexcept { return node_->local_id(); }

uint16_t Server::port() const noexcept { return node_->listen_port(); }

ServerStats Server::stats() const {
    ServerStats stats{
        .peers = node_->peer_count(),
        .registrations = rendezvous_->registrations(),
        .relayed = relay_->carried_circuits(),
        .relayed_bytes = relay_->carried_bytes(),
        .punches = punch_->relayed(),
    };
    const MediaSession& session = media_->session();
    for (const OfferedStream& offered : session.offered()) {
        const std::optional<IncomingStreamStats> stream =
            session.incoming_stats(offered.peer, offered.description.id);
        if (!stream || !stream->subscribed) continue;
        stats.forwarded += 1;
        stats.forwarded_to += stream->relaying_to;
        stats.forwarded_bytes += stream->relayed_bytes;
    }
    return stats;
}

}  // namespace ratcast
