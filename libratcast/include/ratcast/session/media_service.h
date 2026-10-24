#pragma once

/**
 * @file media_service.h
 * @brief A MediaSession on a librats node.
 *
 * Attach it to a node like any other subsystem, before start():
 *
 *     auto* media = node.add_subsystem(std::make_unique<ratcast::MediaService>(handlers));
 *     media->session().add_stream(screen);  // offered to peers as they connect
 *     node.start();
 *
 * It carries the session's messages on librats' MessageType::Media lane —
 * control reliably, frames as live messages — feeds it the node's peer events,
 * path samples (which turns the node's path sampling on) and what became of
 * each frame sent, and ticks it from a thread of its own.
 */

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "ratcast/core/time.h"
#include "ratcast/export.h"
#include "ratcast/session/media_network.h"
#include "ratcast/session/media_session.h"

#include "librats/node/node_context.h"
#include "librats/node/peer_network.h"

namespace ratcast {

class RATCAST_EXPORT MediaService final : public librats::Subsystem, private MediaNetwork {
public:
    explicit MediaService(SessionHandlers handlers, SessionConfig config = {},
                          Duration tick_interval = std::chrono::milliseconds(10));
    ~MediaService() override;

    void attach(librats::NodeContext& ctx) override;
    void start() override;
    void stop() override;

    /// The session, usable from construction on; it reaches peers once the
    /// node has started.
    MediaSession& session() noexcept { return session_; }

private:
    void send_reliable(const PeerId& peer, const Buffer& message) override;
    bool send_live(const PeerId& peer, const Buffer& message, Duration lifetime, uint64_t tag,
                   LiveRank rank) override;
    bool writable(const PeerId& peer) const override;
    size_t max_live_message() const override;
    void set_send_ceiling(const PeerId& peer, uint64_t bits_per_second) override;
    bool relayed(const PeerId& peer) const override;

    void run();

    const Duration tick_interval_;
    /// Set by attach(). Until then there are no peers, so nothing is sent.
    std::atomic<librats::PeerNetwork*> network_{nullptr};
    MediaSession session_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex wait_mutex_;
    std::condition_variable wake_;
};

}  // namespace ratcast
