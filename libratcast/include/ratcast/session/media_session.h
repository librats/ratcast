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
 * for the late ones. A data stream (an MPEG-TS feed) is carried the same way,
 * for a DeliveryBuffer to put in order.
 *
 * Beside streams, a session says who it is (Presence: a name, muted,
 * deafened) to every peer as it connects and whenever that changes, and keeps
 * what each peer said of itself.
 *
 * A share may be played in step by all who watch it, each frame shown
 * everywhere at once (set_playout_need()): each viewer measures its clock
 * against the sender's — probes the sender answers at once, stamped on the
 * clock its frames are captured on (peer_clock.h) — and tells it what it
 * needs from capture to played; the sender tells them all the delay they
 * play at, the most any of them needs and a margin (common_delay.h).
 *
 * A stream added with BitrateLimits is rate-controlled: the session follows
 * the paths to its subscribers (on_peer_path) and tells the encoder the
 * bitrate to aim at through `bitrate_changed` (see RateController). With
 * several subscribers the stream goes at the pace of the slowest.
 *
 * A subscriber's frames may go through another subscriber instead, a relay
 * (relay_through()), taking their copy off the sender's uplink: the relay is
 * told whom to pass the stream on to (Forward), and passes each frame on as
 * it came, behind whose it is, with the time it has left — librats' deadline
 * of it as it arrived, not the stream's lifetime again — keeping each viewer
 * as a sender keeps a subscriber. Everything else stays between the sender
 * and each viewer: subscribing, keyframe requests, playing in step. The
 * relay asks the sender for the keyframes its viewers need and tells it what
 * their paths say (RelayedPath), so that rate control follows them too. A
 * relay that stops watching or goes away leaves its viewers to the sender,
 * which sends them the stream itself again from a keyframe.
 *
 * Threads: every method may be called from any thread. Network events arrive
 * on the network's threads, tick() on a timer's, send_frame() on an
 * encoder's, the rest on the application's. Handlers run on whichever of those
 * caused them, never with a session lock held — so a handler may call back
 * into the session — and must not block: the frame handler in particular
 * runs on a network thread and should only queue the frame for a decoder.
 * Frames of one stream reach the handler one at a time, in decode order —
 * audio and data frames in the order they arrived.
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "ratcast/core/buffer.h"
#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/export.h"
#include "ratcast/session/common_delay.h"
#include "ratcast/session/media_network.h"
#include "ratcast/session/peer_clock.h"
#include "ratcast/session/presence.h"
#include "ratcast/session/rate_controller.h"
#include "ratcast/session/shared_uplink.h"
#include "ratcast/stream/frame.h"
#include "ratcast/stream/frame_tracker.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast {

namespace wire {
struct Forward;
struct RelayedPath;
}  // namespace wire

struct SessionConfig {
    /// Least time between two keyframe requests a receiver sends for one
    /// stream while it cannot decode it. Keep it above min_keyframe_interval:
    /// a sender ignores requests that arrive that soon after a keyframe.
    Duration keyframe_retry = std::chrono::milliseconds(250);
    /// Least time between two keyframes a sender asks its encoder for.
    Duration min_keyframe_interval = std::chrono::milliseconds(200);
    /// Frames a receiver holds per stream while waiting for a gap to fill.
    size_t max_pending_frames = 128;
    /// The clock this node's frames are captured on: what its viewers measure
    /// their clocks against, to play its share in step (common_delay.h). Null:
    /// the session's own. Must outlive the session.
    Clock* capture_clock = nullptr;
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
    /// A peer said who it is, or that something of it changed.
    std::function<void(const PeerId& peer, const Presence& presence)> presence_changed = {};
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
    size_t relayed = 0;               ///< of the subscribers, those a relay passes it on to
};

struct IncomingStreamStats {
    bool subscribed = false;
    uint64_t bytes = 0;  ///< frame message bytes received while subscribed
    FrameTracker::Stats frames;
    /// Passing it on, as a relay (MediaSession::relay_through()).
    size_t relaying_to = 0;         ///< viewers it is passed on to now
    uint64_t relayed_messages = 0;  ///< frame messages passed on
    uint64_t relayed_bytes = 0;     ///< bytes of those messages
    uint64_t relay_held_back = 0;   ///< not passed on: a full queue, or too late
};

/// A stream a peer offers.
struct OfferedStream {
    PeerId peer;
    StreamDescription description;
    friend bool operator==(const OfferedStream&, const OfferedStream&) = default;
};

/// When a sender's viewers all play its share, as this node sees it.
struct CommonPlayout {
    Duration delay{};   ///< from capture to played, the sender's clock: as it says
    Duration offset{};  ///< the same from a capture time on the sender's clock to this one
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

    /// What this node's uplink is known to carry for media, bits per second
    /// (shared_uplink.h); nullopt until the copies are found to fill it.
    std::optional<uint64_t> uplink_capacity() const;

    /// The frames of `stream` to `viewer` go through `relay`, another of its
    /// subscribers, which passes them on; nullopt: straight to it again, from
    /// its next keyframe. Fails with InvalidArgument for a stream that is not
    /// this node's, a viewer or a relay that does not subscribe to it, or a
    /// relay whose own frames come through the viewer.
    Result<void> relay_through(StreamId stream, const PeerId& viewer, std::optional<PeerId> relay);

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

    // ── Who is who ──────────────────────────────────────────────────────────

    /// Say who this node is: to every peer now, and to each as it connects.
    /// Fails with InvalidArgument for a presence that is not valid.
    Result<void> set_presence(const Presence& presence);

    /// What `peer` last said of itself; none before it said anything, or once
    /// it is gone.
    std::optional<Presence> presence(const PeerId& peer) const;

    /// What peers offer now: each stream announced and not withdrawn, by peer
    /// and id.
    std::vector<OfferedStream> offered() const;

    // ── Playing in step ─────────────────────────────────────────────────────

    /// This node plays `peer`'s share in step with the peer's other viewers,
    /// and needs `need` from a frame's capture (the peer's clock) to playing
    /// it (this one's): its clock is measured against the peer's, and what it
    /// needs told, as it changes. nullopt: it no longer plays in step. Nothing
    /// for a peer that is not connected.
    void set_playout_need(const PeerId& peer, std::optional<Duration> need);

    /// When `peer`'s viewers all play its share (common_delay.h); none until
    /// this node's clock is measured against the peer's and the peer said, or
    /// while it says each is to play in its own time.
    std::optional<CommonPlayout> common_playout(const PeerId& peer) const;

    // ── Driven by the network and a timer ───────────────────────────────────

    void on_peer_connected(const PeerId& peer);
    /// What was sent to `peer` lately may be lost (librats' on_peer_rerouted: its
    /// link lost a cross-connect race): the streams offered are announced again
    /// and the subscriptions asked again — what is sent once, on connect.
    void on_peer_rerouted(const PeerId& peer);
    void on_peer_disconnected(const PeerId& peer);
    void on_peer_writable(const PeerId& peer);
    /// A sample of the path to `peer`, every 100 ms or so (librats'
    /// on_peer_path): what rate control steers by.
    void on_peer_path(const PeerId& peer, const PathStats& sample);
    /// What became of a live message to `peer`, sent under `tag`
    /// (MediaNetwork::send_live): delivered, or given up on.
    void on_live_outcome(const PeerId& peer, uint64_t tag, bool delivered);
    /// A message from `peer`; a live one with its `deadline` where the network
    /// says (librats' on_with_deadline), what a frame passed on is given.
    void on_message(const PeerId& peer, std::span<const uint8_t> message,
                    std::optional<Timestamp> deadline = std::nullopt);

    /// Time-based work: give up gaps that waited long enough, repeat keyframe
    /// requests, ask for keyframes that were held back, probe the clocks of
    /// peers played in step. Call every 10 ms or so.
    void tick();

    /// Messages refused as malformed or of an unknown kind, from any peer.
    uint64_t rejected_messages() const noexcept;

private:
    struct PeerState;
    struct Copy;
    struct Fanout;
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
    /// None for a peer not connected.
    std::shared_ptr<PeerState> find_peer(const PeerId& peer) const;
    std::vector<PeerId> connected_peers() const;
    std::vector<std::pair<StreamId, std::shared_ptr<OutgoingStream>>> outgoing_streams() const;
    /// The next slot in the tags of live messages; under mutex_, held uniquely.
    uint16_t take_slot();

    /// A frame of `origin`'s stream, from `from`: the origin itself, or a
    /// relay passing it on.
    void on_frame(const PeerId& origin, const PeerId& from, std::span<const uint8_t> message,
                  std::optional<Timestamp> deadline);
    /// Pass a frame of `origin`'s on to the viewers this node relays `stream`
    /// to, with `left` to go; under the stream's mutex.
    void pass_on(const PeerId& origin, IncomingStream& stream, const FrameInfo& info,
                 std::span<const uint8_t> message, Duration left, Timestamp now);
    /// Ask `origin` for a keyframe of its stream `id` for those this node
    /// passes it on to, at most once a min_keyframe_interval; under the
    /// stream's mutex.
    void ask_keyframe_for_relayed(const PeerId& origin, StreamId id, IncomingStream& stream,
                                  Timestamp now);
    void on_forward(const PeerId& sender, const wire::Forward& forward);
    void on_relayed_path(const PeerId& relay, const wire::RelayedPath& report);
    /// Announce every stream offered to `peer`, and say who this node is.
    void announce_all(const PeerId& peer);
    void on_presence(const PeerId& peer, Presence presence);
    void on_announce(const PeerId& peer, StreamDescription description);
    void on_withdraw(const PeerId& peer, StreamId stream);
    void on_subscribe(const PeerId& peer, StreamId stream);
    void on_unsubscribe(const PeerId& peer, StreamId stream);
    /// `peer` no longer receives `stream`: those it passed it on to get it
    /// from here again, and its relay is told.
    void drop_subscriber(StreamId id, OutgoingStream& stream, const PeerId& peer);
    void on_keyframe_request(const PeerId& peer, StreamId stream);
    void on_clock_echo(const PeerId& peer, Timestamp sent, Timestamp at);
    void on_playout_need(const PeerId& peer, std::optional<Duration> latency);
    void on_playout_delay(const PeerId& peer, std::optional<Duration> delay);

    /// Probe the clock of a peer played in step, and tell it what this node
    /// needs once it can; under the peer's in_step_mutex.
    void probe_clock(PeerState& state, Timestamp now);
    void tell_need(PeerState& state);
    /// The delay to `viewers`; under viewers_mutex_.
    void tell_delay(const std::vector<PeerId>& viewers);

    /// The copies of the rate-controlled streams: each weighed by the most
    /// its stream may take, all of them and by the peer they go to.
    struct Copies {
        double weight = 0;
        std::unordered_map<PeerId, double, PeerId::Hash> paths;

        /// A copy's part of an uplink that carries `uplink`, by bit per second
        /// of its stream's most; nullopt while the uplink is not known.
        std::optional<double> per_most(std::optional<uint64_t> uplink) const;
    };
    /// Takes the streams' locks: never call it holding one.
    Copies copies() const;
    /// Every rate-controlled stream watched comes down to its part of the
    /// uplink, `per_most` by bit per second of its most. Takes the streams'
    /// locks.
    void limit_to_uplink(double per_most);
    /// Each path with copies sent no faster than its part of an uplink that
    /// carries `uplink` (its copies' weight in all of them), so that bursts to
    /// all at once fit it too; no ceiling where the uplink is not known.
    void pace_to_uplink(const Copies& all, std::optional<uint64_t> uplink);

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
    std::unordered_map<PeerId, Presence, PeerId::Hash> presences_;  ///< what peers said
    /// Held while this node's presence is read and sent, so that a peer
    /// connecting as it changes never hears the old one last.
    std::mutex presence_mutex_;
    std::optional<Presence> presence_;  ///< this node's, once said
    /// The next stream's slot in the tags of its frames; never 0, so no tag is.
    uint16_t next_slot_ = 1;

    mutable std::mutex uplink_mutex_;  ///< guards the two below; taken holding no other lock
    SharedUplink uplink_;
    /// The ceilings the network was told, by peer (pace_to_uplink).
    std::unordered_map<PeerId, uint64_t, PeerId::Hash> ceilings_;

    Clock& capture_clock_;
    std::mutex viewers_mutex_;  ///< guards viewers_; taken holding no other lock
    /// This node's viewers that play its share in step.
    CommonDelay viewers_;

    std::atomic<uint64_t> rejected_{0};
};

}  // namespace ratcast
