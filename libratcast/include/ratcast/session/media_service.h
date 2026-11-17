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
 *
 * Every peer that connects is the session's, unless admission is asked for
 * (Admission::Required): then a peer reaches the session only once the
 * application admits it (admit()) — until then it is told of no stream, gets
 * no frame, presence or chat, and what it sends on the lane is dropped. What
 * an application that lets peers in one by one (a remote desktop's host,
 * which asks its user first) needs: a peer not let in costs nothing and learns
 * nothing.
 */

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "ratcast/core/time.h"
#include "ratcast/export.h"
#include "ratcast/session/media_network.h"
#include "ratcast/session/media_session.h"

#include "librats/node/node_context.h"
#include "librats/node/peer_network.h"

namespace ratcast {

/// Whether a peer that connects is the session's at once, or once admitted.
enum class Admission : uint8_t {
    Everyone,  ///< every peer connected
    Required,  ///< only peers admit() let in
};

class RATCAST_EXPORT MediaService final : public librats::Subsystem, private MediaNetwork {
public:
    explicit MediaService(SessionHandlers handlers, SessionConfig config = {},
                          Duration tick_interval = std::chrono::milliseconds(10),
                          Admission admission = Admission::Everyone);
    ~MediaService() override;

    void attach(librats::NodeContext& ctx) override;
    void start() override;
    void stop() override;

    /// The session, usable from construction on; it reaches peers once the
    /// node has started.
    MediaSession& session() noexcept { return session_; }

    /// Let `peer` reach the session (Admission::Required): now if it is
    /// connected, else as it connects; it stays admitted across its
    /// reconnections until dismissed. Any thread; idempotent. Nothing to do
    /// with Admission::Everyone.
    void admit(const PeerId& peer);
    /// Take `peer` back out: the session sees it leave, if it was there, and
    /// hears nothing of it again until admitted again.
    void dismiss(const PeerId& peer);
    /// Whether `peer` reaches the session: admitted, or admission not asked
    /// for.
    bool admitted(const PeerId& peer) const;

private:
    void send_reliable(const PeerId& peer, const Buffer& message) override;
    bool send_live(const PeerId& peer, const Buffer& message, Duration lifetime, uint64_t tag,
                   LiveRank rank) override;
    bool writable(const PeerId& peer) const override;
    size_t max_live_message() const override;
    PeerId local_id() const override;
    void set_send_ceiling(const PeerId& peer, uint64_t bits_per_second) override;
    bool relayed(const PeerId& peer) const override;

    void run();

    /// Whether `peer` is the session's now: admitted and connected, or anyone.
    bool passes(const PeerId& peer) const;

    const Duration tick_interval_;
    const Admission admission_;
    /// The peers admitted, and those connected (Admission::Required).
    mutable std::mutex gate_;
    std::unordered_set<PeerId, PeerId::Hash> admitted_;
    std::unordered_set<PeerId, PeerId::Hash> connected_;
    /// The peers the session was told connected and not yet disconnected.
    std::unordered_set<PeerId, PeerId::Hash> in_session_;
    /// Set by attach(). Until then there are no peers, so nothing is sent.
    std::atomic<librats::PeerNetwork*> network_{nullptr};
    MediaSession session_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex wait_mutex_;
    std::condition_variable wake_;
};

}  // namespace ratcast
