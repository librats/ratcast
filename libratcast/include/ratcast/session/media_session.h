#pragma once

/**
 * @file media_session.h
 * @brief Streams between peers: offering, subscribing, sending, receiving.
 *
 * A MediaSession is one node's side of every media stream it sends or
 * receives. As a sender it announces its streams to every peer, keeps a list
 * of subscribers per stream and sends each frame to each of them as a live
 * message. As a receiver it learns the streams its peers offer, subscribes to
 * those the application asks for, and hands their frames on in decode order
 * with every reference in place (FrameTracker).
 *
 * Keyframes are the currency of recovery. A subscriber starts receiving at the
 * next keyframe; a receiver that loses a frame something depends on asks for
 * one; a subscriber whose queue was full misses frames and gets nothing more
 * until one. The session tells the encoder through `keyframe_needed`, at most
 * once per `min_keyframe_interval` however many receivers ask.
 *
 * Threads: every method may be called from any thread. Network events arrive
 * on the network's threads, tick() on a timer's, send_frame() on an
 * encoder's, the rest on the application's. Handlers run on whichever of those
 * caused them, never with a session lock held — so a handler may call back
 * into the session — and must not block: the frame handler in particular
 * runs on a network thread and should only queue the frame for a decoder.
 * Frames of one stream reach the handler one at a time, in decode order.
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "ratcast/core/buffer.h"
#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/export.h"
#include "ratcast/session/media_network.h"
#include "ratcast/stream/frame.h"
#include "ratcast/stream/frame_tracker.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast {

struct SessionConfig {
    /// Least time between two keyframe requests a receiver sends for one
    /// stream while it cannot decode it. Keep it above min_keyframe_interval:
    /// a sender ignores requests that arrive that soon after a keyframe.
    Duration keyframe_retry = std::chrono::milliseconds(250);
    /// Least time between two keyframes a sender asks its encoder for.
    Duration min_keyframe_interval = std::chrono::milliseconds(200);
    /// Frames a receiver holds per stream while waiting for a gap to fill.
    size_t max_pending_frames = 128;
};

struct SessionHandlers {
    /// A peer offers a stream, or changed the description of one it offers.
    std::function<void(const PeerId& peer, const StreamDescription& stream)> stream_announced = {};
    /// A peer no longer offers a stream: it withdrew it or disconnected.
    std::function<void(const PeerId& peer, StreamId stream)> stream_withdrawn = {};
    /// The next frame of a subscribed stream, in decode order and decodable.
    std::function<void(const PeerId& peer, EncodedFrame frame)> frame_received = {};
    /// The encoder of a local stream should make its next frame a keyframe.
    std::function<void(StreamId stream)> keyframe_needed = {};
};

struct OutgoingStreamStats {
    size_t subscribers = 0;
    uint64_t frames = 0;              ///< frames given to send_frame()
    uint64_t keyframes = 0;           ///< of which keyframes
    uint64_t messages = 0;            ///< frame messages sent: one per frame per subscriber
    uint64_t bytes = 0;               ///< bytes of those messages
    uint64_t awaiting_keyframe = 0;   ///< not sent: the subscriber waits for a keyframe
    uint64_t backpressure_drops = 0;  ///< not sent: the subscriber's queue was full
    uint64_t keyframe_requests = 0;   ///< keyframe requests received from subscribers
    uint64_t keyframes_asked = 0;     ///< times keyframe_needed fired
};

struct IncomingStreamStats {
    bool subscribed = false;
    uint64_t bytes = 0;  ///< frame message bytes received while subscribed
    FrameTracker::Stats frames;
};

class RATCAST_EXPORT MediaSession {
public:
    /// `network` and `clock` must outlive the session.
    MediaSession(MediaNetwork& network, Clock& clock, SessionHandlers handlers,
                 SessionConfig config = {});
    ~MediaSession();
    MediaSession(const MediaSession&) = delete;
    MediaSession& operator=(const MediaSession&) = delete;

    // ── Streams this node sends ─────────────────────────────────────────────

    /// Offer a stream to every peer, now and as they connect. Fails with
    /// InvalidArgument for a description that is not valid or an id in use.
    Result<void> add_stream(const StreamDescription& description);

    /// Replace a stream's description and announce it again.
    Result<void> update_stream(const StreamDescription& description);

    /// Stop offering a stream; its subscribers are told it is gone.
    void remove_stream(StreamId stream);

    /// Send a frame of `stream` to its subscribers. `info.stream` is set from
    /// `stream`; frame numbers must increase. A payload from
    /// allocate_frame_payload() is sent without being copied. Fails with
    /// InvalidArgument for an unknown stream, a frame number that does not
    /// increase, frame metadata that cannot be encoded, or a frame too large
    /// for the network.
    Result<void> send_frame(StreamId stream, FrameInfo info, MutableBuffer payload);

    std::optional<OutgoingStreamStats> outgoing_stats(StreamId stream) const;

    // ── Streams peers send ──────────────────────────────────────────────────

    /// Receive `peer`'s `stream`, from its next keyframe on. Fails with
    /// InvalidArgument if the peer has not announced that stream.
    Result<void> subscribe(const PeerId& peer, StreamId stream);

    void unsubscribe(const PeerId& peer, StreamId stream);

    /// Ask for a keyframe of a subscribed stream, after a decoder error. Until
    /// it comes, frames that depend on earlier ones are dropped, and the
    /// request is repeated as it is for a lost frame.
    void request_keyframe(const PeerId& peer, StreamId stream);

    std::optional<IncomingStreamStats> incoming_stats(const PeerId& peer, StreamId stream) const;

    // ── Driven by the network and a timer ───────────────────────────────────

    void on_peer_connected(const PeerId& peer);
    void on_peer_disconnected(const PeerId& peer);
    void on_peer_writable(const PeerId& peer);
    void on_message(const PeerId& peer, std::span<const uint8_t> message);

    /// Time-based work: give up gaps that waited long enough, repeat keyframe
    /// requests, ask for keyframes that were held back. Call every 10 ms or so.
    void tick();

    /// Messages refused as malformed or of an unknown kind, from any peer.
    uint64_t rejected_messages() const noexcept;

private:
    struct PeerState;
    struct OutgoingStream;
    struct IncomingStream;

    struct StreamKey {
        PeerId peer;
        StreamId stream = 0;
        friend bool operator==(const StreamKey&, const StreamKey&) = default;
    };
    struct StreamKeyHash {
        size_t operator()(const StreamKey& key) const noexcept;
    };

    std::shared_ptr<OutgoingStream> find_outgoing(StreamId stream) const;
    std::shared_ptr<IncomingStream> find_incoming(const PeerId& peer, StreamId stream) const;
    std::shared_ptr<PeerState> peer_state(const PeerId& peer);
    std::vector<PeerId> connected_peers() const;

    void on_frame(const PeerId& peer, std::span<const uint8_t> message);
    void on_announce(const PeerId& peer, StreamDescription description);
    void on_withdraw(const PeerId& peer, StreamId stream);
    void on_subscribe(const PeerId& peer, StreamId stream);
    void on_unsubscribe(const PeerId& peer, StreamId stream);
    void on_keyframe_request(const PeerId& peer, StreamId stream);

    void deliver_ready(const PeerId& peer, StreamId id, IncomingStream& stream, Timestamp now);
    bool ask_keyframe(OutgoingStream& stream, Timestamp now);
    FrameTracker::Config tracker_config(const StreamDescription& description) const;

    MediaNetwork& network_;
    Clock& clock_;
    const SessionHandlers handlers_;
    const SessionConfig config_;

    /// Guards the maps — which streams and peers exist — but not the streams
    /// themselves, each of which has a lock of its own.
    mutable std::shared_mutex mutex_;
    std::unordered_map<StreamId, std::shared_ptr<OutgoingStream>> outgoing_;
    std::unordered_map<StreamKey, std::shared_ptr<IncomingStream>, StreamKeyHash> incoming_;
    std::unordered_map<PeerId, std::shared_ptr<PeerState>, PeerId::Hash> peers_;

    std::atomic<uint64_t> rejected_{0};
};

}  // namespace ratcast
