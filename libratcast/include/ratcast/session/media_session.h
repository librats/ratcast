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
 * next keyframe; a frame to it that the network gave up on (on_live_outcome)
 * means it cannot decode what follows, and so does one a receiver finds
 * missing, which it asks for a keyframe for; a subscriber whose queue was full
 * misses frames and gets nothing more until one. The session tells the encoder
 * through `keyframe_needed`, at most once per `min_keyframe_interval` however
 * many receivers ask.
 *
 * Joining is the exception: a subscriber's first keyframe may arrive late
 * rather than not at all. A fresh connection's window opens over a few round
 * trips, and a keyframe sent again meets the same window, so asking again
 * gains nothing. The first keyframe, and the frames behind it, go with four
 * round trips more than the stream's lifetime, shrinking back at half the time
 * passing so that each frame is still due after the one before it; the viewer
 * plays them catching up. Until the network says the keyframe arrived, the
 * receiver's requests for one are not news; if it was given up on, it is sent
 * again.
 *
 * Audio is the other exception: each of its frames decodes on its own, so a
 * subscriber starts at once, never needs a keyframe, and a frame to it that
 * the network gave up on costs only itself. A receiver hands audio frames on
 * as they arrive, for a jitter buffer (JitterBuffer) to put in order and wait
 * for the late ones.
 *
 * A stream added with BitrateLimits is rate-controlled: the session follows
 * the paths to its subscribers (on_peer_path) and tells the encoder the
 * bitrate to aim at through `bitrate_changed` (see RateController). With
 * several subscribers the stream goes at the pace of the slowest.
 *
 * Threads: every method may be called from any thread. Network events arrive
 * on the network's threads, tick() on a timer's, send_frame() on an
 * encoder's, the rest on the application's. Handlers run on whichever of those
 * caused them, never with a session lock held — so a handler may call back
 * into the session — and must not block: the frame handler in particular
 * runs on a network thread and should only queue the frame for a decoder.
 * Frames of one stream reach the handler one at a time, in decode order —
 * audio frames in the order they arrived.
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
#include "ratcast/session/rate_controller.h"
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
    /// The encoder of a rate-controlled local stream should aim at a new
    /// bitrate, in bits per second.
    std::function<void(StreamId stream, uint32_t bits_per_second)> bitrate_changed = {};
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
    uint32_t bitrate = 0;             ///< what rate control aims at now; 0 without it
    uint64_t bitrate_cuts = 0;        ///< times rate control lowered the bitrate
    uint64_t bitrate_raises = 0;      ///< and raised it
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

    /// Offer a stream to every peer, now and as they connect; with `bitrate`,
    /// rate-controlled from `bitrate.start` on. Fails with InvalidArgument for
    /// a description or limits that are not valid, or an id in use.
    Result<void> add_stream(const StreamDescription& description,
                            std::optional<BitrateLimits> bitrate = std::nullopt);

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
    /// A sample of the path to `peer`, every 100 ms or so (librats'
    /// on_peer_path): what rate control steers by.
    void on_peer_path(const PeerId& peer, const PathStats& sample);
    /// What became of a live message to `peer`, sent under `tag`
    /// (MediaNetwork::send_live): delivered, or given up on.
    void on_live_outcome(const PeerId& peer, uint64_t tag, bool delivered);
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
    /// The next stream's slot in the tags of its frames; never 0, so no tag is.
    uint16_t next_slot_ = 1;

    std::atomic<uint64_t> rejected_{0};
};

}  // namespace ratcast
