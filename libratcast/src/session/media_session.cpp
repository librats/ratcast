#include "ratcast/session/media_session.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "wire/control_codec.h"
#include "wire/frame_codec.h"
#include "wire/message.h"

namespace ratcast {
namespace {

using namespace std::chrono_literals;

/// A joining subscriber's first keyframe may take this many round trips more
/// than the stream's lifetime: a fresh connection's window, doubling each round
/// trip from a few packets, has let some 60 through by then — a 1080p keyframe
/// at the bitrate a stream starts at, and the frames behind it.
constexpr int kJoinRoundTrips = 4;
/// The round trip taken before the path's first sample.
constexpr Duration kUnknownRtt = 100ms;

/// The clock of a peer played in step is probed this often, after
/// kQuickProbes quicker ones, for a first measure soon.
constexpr Duration kProbeEvery = 1s;
constexpr Duration kQuickProbeEvery = 200ms;
constexpr int kQuickProbes = 5;
/// A need that moved less than this since it was told is no news.
constexpr Duration kNeedNews = 5ms;

/// A frame's tag in the network's outcomes: its stream's slot, above its number.
constexpr int kSlotShift = 48;
constexpr uint64_t kNumberMask = (uint64_t{1} << kSlotShift) - 1;

uint64_t live_tag(uint16_t slot, FrameNumber number) noexcept {
    return (uint64_t{slot} << kSlotShift) | ((number + 1) & kNumberMask);
}

}  // namespace

// ── Per-peer and per-stream state ───────────────────────────────────────────

struct MediaSession::PeerState {
    explicit PeerState(PeerId peer_id) : id(std::move(peer_id)) {}

    const PeerId id;
    /// The peer's queue was full at the last send; nothing goes to it until it
    /// drains. Set by senders, cleared by the network's writable event or by
    /// tick() seeing the peer writable again.
    std::atomic<bool> blocked{false};

    std::mutex path_mutex;  ///< guards `path`
    PathMonitor path;
    /// The smoothed round trip of the last path sample; 0 before the first.
    std::atomic<uint32_t> rtt_us{0};

    Duration rtt() const noexcept {
        const uint32_t us = rtt_us.load(std::memory_order_relaxed);
        return us == 0 ? kUnknownRtt : Duration{us};
    }

    /// This node plays the peer's share in step (set_playout_need()).
    struct InStep {
        Duration need{};                ///< capture to played, this node's clock
        std::optional<Duration> told;   ///< the latency told the peer, its clock
        std::optional<Duration> delay;  ///< what the peer says its viewers play at
        PeerClock clock;
        Timestamp next_probe{};
        int probes = 0;
    };
    std::mutex in_step_mutex;  ///< guards `in_step`
    std::optional<InStep> in_step;
};

struct MediaSession::OutgoingStream {
    struct Subscriber {
        explicit Subscriber(std::shared_ptr<PeerState> state) : peer(std::move(state)) {}

        std::shared_ptr<PeerState> peer;
        /// Missed a frame (or has seen none yet): the frames before the next
        /// keyframe are of no use to it.
        bool awaiting_keyframe = true;
        /// A frame was held back for it since its path's last sample: its
        /// queue was full, which rate control takes for overuse.
        bool backpressured = false;
        /// What its path is known to carry, as of the last sample.
        std::optional<uint64_t> capacity;

        /// A keyframe of the stream is known to have reached it.
        bool joined = false;
        /// Its first keyframe, on its way: none before it is sent, nor once the
        /// network has said what became of it.
        std::optional<FrameNumber> joining;
        /// What the last frame to it was given, and when.
        Duration lifetime{};
        Timestamp sent_at{};
    };

    OutgoingStream(StreamDescription d, std::optional<BitrateLimits> bitrate, uint16_t tag_slot)
        : slot(tag_slot), description(std::move(d)) {
        if (bitrate) rate.emplace(*bitrate, description.lifetime);
    }

    /// Every frame of it decodes on its own — sound, or bytes ratcast does not
    /// look into: nothing waits for a keyframe, and no frame is worth more
    /// time than the others.
    bool stands_alone() const noexcept { return description.kind() != MediaKind::Video; }

    /// Where a frame goes among the live messages to a peer: sound first, then
    /// a keyframe, then the frames that depend on it, and data with them.
    LiveRank rank_of(const FrameInfo& info) const noexcept {
        switch (description.kind()) {
            case MediaKind::Audio: return LiveRank::Sound;
            case MediaKind::Data: return LiveRank::Frame;
            case MediaKind::Video: break;
        }
        return info.keyframe ? LiveRank::Keyframe : LiveRank::Frame;
    }

    /// How long `subscriber` is given for a frame sent now. Its first keyframe
    /// gets kJoinRoundTrips round trips more than the stream's lifetime; the
    /// frames after it get what the one before them got, less half the time
    /// since, so each is still due after the one before it.
    Duration lifetime_for(Subscriber& subscriber, const FrameInfo& info, Timestamp now) {
        if (stands_alone()) return description.lifetime;
        if (!subscriber.joined && info.keyframe && subscriber.awaiting_keyframe) {
            subscriber.joining = info.number;
            subscriber.lifetime = description.lifetime + kJoinRoundTrips * subscriber.peer->rtt();
        } else {
            subscriber.lifetime = std::max(description.lifetime,
                                           subscriber.lifetime - (now - subscriber.sent_at) / 2);
        }
        subscriber.sent_at = now;
        return subscriber.lifetime;
    }

    /// The network settled frame `number` to `subscriber`. Returns whether that
    /// leaves it needing a keyframe.
    bool settle(Subscriber& subscriber, FrameNumber number, bool delivered) {
        if (subscriber.joining && number == *subscriber.joining) {
            subscriber.joining.reset();
            if (delivered) {
                subscriber.joined = true;
                return false;
            }
            subscriber.awaiting_keyframe = true;  // what follows it is of no use
            return true;
        }
        // What follows a frame given up on cannot be decoded, up to a keyframe
        // sent after it. Behind a joining keyframe still on its way too: asked
        // for now, the next one goes right behind it, as late as it may be.
        return !delivered && (subscriber.joined || subscriber.joining.has_value()) &&
               !(last_keyframe && *last_keyframe > number);
    }

    const uint16_t slot;  ///< in the tags of its frames
    std::mutex mutex;     ///< guards everything below
    StreamDescription description;
    std::vector<Subscriber> subscribers;
    std::optional<FrameNumber> last_number;
    std::optional<FrameNumber> last_keyframe;
    std::optional<Timestamp> last_keyframe_sent;
    std::optional<Timestamp> last_keyframe_asked;
    /// A keyframe was needed within min_keyframe_interval of the last one; tick()
    /// asks for it once the interval has passed, unless a keyframe came first.
    bool keyframe_wanted = false;
    std::optional<RateController> rate;  ///< for a rate-controlled stream
    OutgoingStreamStats stats;

    Subscriber* subscriber_of(const PeerId& peer) {
        const auto it = std::ranges::find_if(
            subscribers, [&](const Subscriber& subscriber) { return subscriber.peer->id == peer; });
        return it == subscribers.end() ? nullptr : &*it;
    }

    /// Counts a change rate control made and passes it on.
    std::optional<uint32_t> count(std::optional<uint32_t> bitrate, uint32_t before) {
        if (bitrate) (*bitrate < before ? stats.bitrate_cuts : stats.bitrate_raises) += 1;
        return bitrate;
    }

    /// Rate control hears a sample of `subscriber`'s path, of which a copy may
    /// take at most `uplink` where the uplink they all go through is known.
    std::optional<uint32_t> follow(Subscriber& subscriber, PathSignal path,
                                   std::optional<uint64_t> uplink, Timestamp now) {
        if (uplink) path.capacity = std::min(path.capacity.value_or(*uplink), *uplink);
        subscriber.capacity = path.capacity;
        const uint32_t before = rate->target();
        if (std::exchange(subscriber.backpressured, false) || rate->overused(path)) {
            return count(rate->on_overuse(now, path), before);
        }
        // Raised no further than the narrowest path is known to carry.
        std::optional<uint64_t> capacity;
        for (const Subscriber& other : subscribers) {
            if (other.capacity)
                capacity = std::min(capacity.value_or(*other.capacity), *other.capacity);
        }
        return count(rate->on_clear(now, capacity), before);
    }

    /// Nobody watches any more: the next viewer starts afresh. Neither a cut
    /// nor a raise: no path asked for it.
    std::optional<uint32_t> restart_if_unwatched() {
        if (!rate || !subscribers.empty()) return std::nullopt;
        return rate->restart();
    }
};

struct MediaSession::IncomingStream {
    IncomingStream(StreamDescription d, FrameTracker::Config config)
        : description(std::move(d)), tracker(config) {}

    /// Held while frames are handed to the handler, so that they leave one at
    /// a time and in order whichever thread made them ready. Always taken
    /// before `mutex`, never while holding it.
    std::mutex delivery;
    std::mutex mutex;  ///< guards everything below
    StreamDescription description;
    bool subscribed = false;
    FrameTracker tracker;
    uint64_t bytes = 0;
};

size_t MediaSession::StreamKeyHash::operator()(const StreamKey& key) const noexcept {
    return PeerId::Hash{}(key.peer) ^ (size_t{key.stream} * 0x9E3779B97F4A7C15ull);
}

namespace {

template <typename Handler, typename... Args>
void notify(const Handler& handler, Args&&... args) {
    if (handler) handler(std::forward<Args>(args)...);
}

}  // namespace

MediaSession::MediaSession(MediaNetwork& network, Clock& clock, SessionHandlers handlers,
                           SessionConfig config)
    : network_(network),
      clock_(clock),
      handlers_(std::move(handlers)),
      config_(config),
      capture_clock_(config.capture_clock ? *config.capture_clock : clock) {}

MediaSession::~MediaSession() = default;

// ── Lookups ─────────────────────────────────────────────────────────────────

std::shared_ptr<MediaSession::OutgoingStream> MediaSession::find_outgoing(StreamId stream) const {
    std::shared_lock lock(mutex_);
    const auto it = outgoing_.find(stream);
    return it == outgoing_.end() ? nullptr : it->second;
}

std::shared_ptr<MediaSession::IncomingStream> MediaSession::find_incoming(const PeerId& peer,
                                                                          StreamId stream) const {
    std::shared_lock lock(mutex_);
    const auto it = incoming_.find(StreamKey{peer, stream});
    return it == incoming_.end() ? nullptr : it->second;
}

std::shared_ptr<MediaSession::PeerState> MediaSession::peer_state(const PeerId& peer) {
    {
        std::shared_lock lock(mutex_);
        if (const auto it = peers_.find(peer); it != peers_.end()) return it->second;
    }
    // A message can overtake the connection event that announces its peer.
    std::unique_lock lock(mutex_);
    auto& state = peers_[peer];
    if (!state) state = std::make_shared<PeerState>(peer);
    return state;
}

std::shared_ptr<MediaSession::PeerState> MediaSession::find_peer(const PeerId& peer) const {
    std::shared_lock lock(mutex_);
    const auto it = peers_.find(peer);
    return it == peers_.end() ? nullptr : it->second;
}

std::vector<PeerId> MediaSession::connected_peers() const {
    std::shared_lock lock(mutex_);
    std::vector<PeerId> peers;
    peers.reserve(peers_.size());
    for (const auto& [id, state] : peers_) peers.push_back(id);
    return peers;
}

FrameTracker::Config MediaSession::tracker_config(const StreamDescription& description) const {
    return FrameTracker::Config{
        .patience = description.lifetime,
        .keyframe_retry = config_.keyframe_retry,
        .max_pending = config_.max_pending_frames,
        .in_order = description.kind() == MediaKind::Video,
    };
}

// ── Streams this node sends ─────────────────────────────────────────────────

Result<void> MediaSession::add_stream(const StreamDescription& description,
                                      std::optional<BitrateLimits> bitrate) {
    if (!is_valid(description) || (bitrate && !is_valid(*bitrate))) {
        return fail(Errc::InvalidArgument);
    }

    std::vector<PeerId> peers;
    {
        std::unique_lock lock(mutex_);
        if (outgoing_.contains(description.id)) return fail(Errc::InvalidArgument);
        outgoing_.emplace(description.id,
                          std::make_shared<OutgoingStream>(description, bitrate, next_slot_));
        if (++next_slot_ == 0) next_slot_ = 1;
        // Taken under the same lock as the insertion: a peer connecting
        // concurrently is either listed here or sees the stream itself.
        for (const auto& [id, state] : peers_) peers.push_back(id);
    }

    const Buffer announce = wire::encode_control(wire::Announce{description});
    for (const PeerId& peer : peers) network_.send_reliable(peer, announce);
    return {};
}

Result<void> MediaSession::update_stream(const StreamDescription& description) {
    if (!is_valid(description)) return fail(Errc::InvalidArgument);
    const std::shared_ptr<OutgoingStream> stream = find_outgoing(description.id);
    if (!stream) return fail(Errc::InvalidArgument);
    {
        std::lock_guard lock(stream->mutex);
        stream->description = description;
    }

    const Buffer announce = wire::encode_control(wire::Announce{description});
    for (const PeerId& peer : connected_peers()) network_.send_reliable(peer, announce);
    return {};
}

void MediaSession::remove_stream(StreamId stream) {
    std::vector<PeerId> peers;
    {
        std::unique_lock lock(mutex_);
        if (outgoing_.erase(stream) == 0) return;
        for (const auto& [id, state] : peers_) peers.push_back(id);
    }
    const Buffer withdraw = wire::encode_control(wire::Withdraw{stream});
    for (const PeerId& peer : peers) network_.send_reliable(peer, withdraw);
}

Result<void> MediaSession::send_frame(StreamId stream_id, FrameInfo info, MutableBuffer payload) {
    const std::shared_ptr<OutgoingStream> stream = find_outgoing(stream_id);
    if (!stream) return fail(Errc::InvalidArgument);
    info.stream = stream_id;
    if (!wire::is_encodable(info)) return fail(Errc::InvalidArgument);
    if (wire::frame_header_size(info) + payload.size() > network_.max_live_message()) {
        return fail(Errc::InvalidArgument);
    }

    const Timestamp now = clock_.now();
    bool ask = false;
    {
        std::lock_guard lock(stream->mutex);
        OutgoingStream& s = *stream;
        if (s.last_number && info.number <= *s.last_number) return fail(Errc::InvalidArgument);
        s.last_number = info.number;

        s.stats.frames += 1;
        if (info.keyframe) {
            s.stats.keyframes += 1;
            s.last_keyframe_sent = now;
            s.last_keyframe = info.number;
            s.keyframe_wanted = false;
        }

        // Encoded on first use: once for every subscriber, never when none can use it.
        std::optional<Buffer> message;

        for (OutgoingStream::Subscriber& subscriber : s.subscribers) {
            const bool useful = info.keyframe || !subscriber.awaiting_keyframe;
            if (subscriber.peer->blocked.load(std::memory_order_acquire)) {
                // Everything after this frame may reference it, so the subscriber
                // can only pick up again at a keyframe. Not asked for yet: it could
                // not be sent either, and keyframes are the last thing a full
                // queue needs. The first frame after it drains asks.
                if (useful) {
                    subscriber.awaiting_keyframe = true;
                    subscriber.backpressured = true;
                    s.stats.backpressure_drops += 1;
                } else {
                    s.stats.awaiting_keyframe += 1;
                }
                continue;
            }
            if (!useful) {
                s.stats.awaiting_keyframe += 1;
                ask = ask_keyframe(s, now) || ask;
                continue;
            }

            if (!message) {
                Result<Buffer> encoded = wire::encode_frame(info, std::move(payload));
                if (!encoded) return fail(encoded.error());
                message = std::move(*encoded);
            }
            const Duration lifetime = s.lifetime_for(subscriber, info, now);
            subscriber.awaiting_keyframe = false;
            s.stats.messages += 1;
            s.stats.bytes += message->size();
            // What became of a frame that stands alone changes nothing.
            const uint64_t tag = s.stands_alone() ? 0 : live_tag(s.slot, info.number);
            if (!network_.send_live(subscriber.peer->id, *message, lifetime, tag,
                                    s.rank_of(info))) {
                subscriber.peer->blocked.store(true, std::memory_order_release);
                subscriber.backpressured = true;
            }
        }
    }
    if (ask) notify(handlers_.keyframe_needed, stream_id);
    return {};
}

// Whether to tell the encoder now. Within min_keyframe_interval of the last
// keyframe asked for or sent, the request is held for tick() instead.
bool MediaSession::ask_keyframe(OutgoingStream& stream, Timestamp now) {
    const std::optional<Timestamp> since =
        std::max(stream.last_keyframe_asked, stream.last_keyframe_sent);
    if (since && now - *since < config_.min_keyframe_interval) {
        stream.keyframe_wanted = true;
        return false;
    }
    stream.keyframe_wanted = false;
    stream.last_keyframe_asked = now;
    stream.stats.keyframes_asked += 1;
    return true;
}

std::optional<uint64_t> MediaSession::uplink_capacity() const {
    std::lock_guard lock(uplink_mutex_);
    return uplink_.capacity();
}

MediaSession::Copies MediaSession::copies() const {
    std::vector<std::shared_ptr<OutgoingStream>> streams;
    {
        std::shared_lock lock(mutex_);
        for (const auto& [id, stream] : outgoing_) streams.push_back(stream);
    }
    Copies copies;
    for (const auto& stream : streams) {
        std::lock_guard lock(stream->mutex);
        if (!stream->rate) continue;
        const double max = stream->rate->limits().max;
        copies.weight += max * double(stream->subscribers.size());
        for (const auto& subscriber : stream->subscribers) copies.paths[subscriber.peer->id] += max;
    }
    return copies;
}

std::optional<double> MediaSession::Copies::per_most(std::optional<uint64_t> uplink) const {
    if (!uplink || weight <= 0) return std::nullopt;
    return double(*uplink) * SharedUplink::kCopies / weight;
}

std::optional<OutgoingStreamStats> MediaSession::outgoing_stats(StreamId stream_id) const {
    const std::shared_ptr<OutgoingStream> stream = find_outgoing(stream_id);
    if (!stream) return std::nullopt;
    std::lock_guard lock(stream->mutex);
    OutgoingStreamStats stats = stream->stats;
    stats.subscribers = stream->subscribers.size();
    if (stream->rate) stats.bitrate = stream->rate->target();
    return stats;
}

// ── Streams peers send ──────────────────────────────────────────────────────

Result<void> MediaSession::subscribe(const PeerId& peer, StreamId stream_id) {
    const std::shared_ptr<IncomingStream> stream = find_incoming(peer, stream_id);
    if (!stream) return fail(Errc::InvalidArgument);
    {
        std::lock_guard lock(stream->mutex);
        if (stream->subscribed) return {};
        stream->subscribed = true;
        stream->tracker.reset(clock_.now());
    }
    network_.send_reliable(peer, wire::encode_control(wire::Subscribe{stream_id}));
    return {};
}

void MediaSession::unsubscribe(const PeerId& peer, StreamId stream_id) {
    const std::shared_ptr<IncomingStream> stream = find_incoming(peer, stream_id);
    if (!stream) return;
    {
        std::lock_guard lock(stream->mutex);
        if (!stream->subscribed) return;
        stream->subscribed = false;
        stream->tracker.reset(clock_.now());  // let go of frames held for it
    }
    network_.send_reliable(peer, wire::encode_control(wire::Unsubscribe{stream_id}));
}

void MediaSession::request_keyframe(const PeerId& peer, StreamId stream_id) {
    const std::shared_ptr<IncomingStream> stream = find_incoming(peer, stream_id);
    if (!stream) return;
    {
        std::lock_guard lock(stream->mutex);
        if (!stream->subscribed) return;
        stream->tracker.invalidate(clock_.now());  // tick() repeats the request until one comes
    }
    network_.send_reliable(peer, wire::encode_control(wire::KeyframeRequest{stream_id}));
}

std::optional<IncomingStreamStats> MediaSession::incoming_stats(const PeerId& peer,
                                                                StreamId stream_id) const {
    const std::shared_ptr<IncomingStream> stream = find_incoming(peer, stream_id);
    if (!stream) return std::nullopt;
    std::lock_guard lock(stream->mutex);
    return IncomingStreamStats{stream->subscribed, stream->bytes, stream->tracker.stats()};
}

std::vector<OfferedStream> MediaSession::offered() const {
    std::vector<std::pair<PeerId, std::shared_ptr<IncomingStream>>> streams;
    {
        std::shared_lock lock(mutex_);
        streams.reserve(incoming_.size());
        for (const auto& [key, stream] : incoming_) streams.emplace_back(key.peer, stream);
    }
    std::vector<OfferedStream> offered;
    offered.reserve(streams.size());
    for (const auto& [peer, stream] : streams) {
        std::lock_guard lock(stream->mutex);
        offered.push_back({peer, stream->description});
    }
    std::ranges::sort(offered, [](const OfferedStream& a, const OfferedStream& b) {
        return a.peer < b.peer || (a.peer == b.peer && a.description.id < b.description.id);
    });
    return offered;
}

// ── Network events ──────────────────────────────────────────────────────────

void MediaSession::on_peer_connected(const PeerId& peer) {
    {
        std::unique_lock lock(mutex_);
        auto& state = peers_[peer];
        if (!state) state = std::make_shared<PeerState>(peer);
    }
    announce_all(peer);
}

void MediaSession::on_peer_rerouted(const PeerId& peer) {
    announce_all(peer);
    std::vector<std::pair<StreamId, std::shared_ptr<IncomingStream>>> streams;
    {
        std::shared_lock lock(mutex_);
        for (const auto& [key, stream] : incoming_) {
            if (key.peer == peer) streams.emplace_back(key.stream, stream);
        }
    }
    // A subscription the peer has already is no news to it.
    for (const auto& [id, stream] : streams) {
        std::unique_lock lock(stream->mutex);
        const bool subscribed = stream->subscribed;
        lock.unlock();
        if (subscribed) network_.send_reliable(peer, wire::encode_control(wire::Subscribe{id}));
    }
    // What this node needs of the peer's share, and the delay the peer's
    // viewers play this node's at.
    if (const std::shared_ptr<PeerState> state = find_peer(peer)) {
        std::lock_guard lock(state->in_step_mutex);
        if (state->in_step) {
            state->in_step->told.reset();
            tell_need(*state);
        }
    }
    std::lock_guard lock(viewers_mutex_);
    if (viewers_.plays(peer) && viewers_.delay()) tell_delay({peer});
}

void MediaSession::announce_all(const PeerId& peer) {
    std::vector<std::shared_ptr<OutgoingStream>> streams;
    {
        std::shared_lock lock(mutex_);
        streams.reserve(outgoing_.size());
        for (const auto& [id, stream] : outgoing_) streams.push_back(stream);
    }
    for (const auto& stream : streams) {
        std::unique_lock lock(stream->mutex);
        wire::Announce announce{stream->description};
        lock.unlock();
        network_.send_reliable(peer, wire::encode_control(announce));
    }
    std::lock_guard lock(presence_mutex_);
    if (presence_) {
        network_.send_reliable(peer, wire::encode_control(wire::PresenceUpdate{*presence_}));
    }
}

Result<void> MediaSession::set_presence(const Presence& presence) {
    if (!is_valid(presence)) return fail(Errc::InvalidArgument);
    const Buffer message = wire::encode_control(wire::PresenceUpdate{presence});
    std::lock_guard lock(presence_mutex_);
    presence_ = presence;
    for (const PeerId& peer : connected_peers()) network_.send_reliable(peer, message);
    return {};
}

std::optional<Presence> MediaSession::presence(const PeerId& peer) const {
    std::shared_lock lock(mutex_);
    const auto it = presences_.find(peer);
    if (it == presences_.end()) return std::nullopt;
    return it->second;
}

void MediaSession::on_presence(const PeerId& peer, Presence presence) {
    {
        std::unique_lock lock(mutex_);
        if (!peers_.contains(peer)) return;  // gone while it was on its way
        presences_[peer] = presence;
    }
    notify(handlers_.presence_changed, peer, presence);
}

void MediaSession::on_peer_disconnected(const PeerId& peer) {
    std::vector<std::pair<StreamId, std::shared_ptr<OutgoingStream>>> streams;
    std::vector<StreamId> withdrawn;
    {
        std::unique_lock lock(mutex_);
        peers_.erase(peer);
        presences_.erase(peer);
        for (auto it = incoming_.begin(); it != incoming_.end();) {
            if (it->first.peer == peer) {
                withdrawn.push_back(it->first.stream);
                it = incoming_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [id, stream] : outgoing_) streams.emplace_back(id, stream);
    }
    {
        std::lock_guard lock(uplink_mutex_);
        uplink_.forget(peer);
        ceilings_.erase(peer);  // the network forgot it too
    }
    {
        std::lock_guard lock(viewers_mutex_);
        if (viewers_.need(peer, std::nullopt, clock_.now())) tell_delay(viewers_.viewers());
    }
    for (const auto& [id, stream] : streams) {
        std::optional<uint32_t> bitrate;
        {
            std::lock_guard lock(stream->mutex);
            const size_t erased = std::erase_if(stream->subscribers, [&](const auto& subscriber) {
                return subscriber.peer->id == peer;
            });
            if (erased > 0) bitrate = stream->restart_if_unwatched();
        }
        if (bitrate) notify(handlers_.bitrate_changed, id, *bitrate);
    }
    for (StreamId stream : withdrawn) notify(handlers_.stream_withdrawn, peer, stream);
}

void MediaSession::on_peer_writable(const PeerId& peer) {
    std::shared_lock lock(mutex_);
    if (const auto it = peers_.find(peer); it != peers_.end()) {
        it->second->blocked.store(false, std::memory_order_release);
    }
}

void MediaSession::on_peer_path(const PeerId& peer, const PathStats& sample) {
    const Timestamp now = clock_.now();
    std::shared_ptr<PeerState> state;
    std::vector<std::pair<StreamId, std::shared_ptr<OutgoingStream>>> streams;
    {
        std::shared_lock lock(mutex_);
        const auto it = peers_.find(peer);
        if (it == peers_.end()) return;
        state = it->second;
        for (const auto& [id, stream] : outgoing_) streams.emplace_back(id, stream);
    }
    state->rtt_us.store(sample.srtt_us, std::memory_order_relaxed);
    PathSignal signal;
    {
        std::lock_guard lock(state->path_mutex);
        signal = state->path.update(sample, now);
    }

    // The rate-controlled streams the peer watches share its path, each in
    // proportion to the most it may take.
    double most = 0;
    bool overused = false;
    std::erase_if(streams, [&](const auto& entry) {
        std::lock_guard lock(entry.second->mutex);
        OutgoingStream& s = *entry.second;
        const OutgoingStream::Subscriber* subscriber = s.rate ? s.subscriber_of(peer) : nullptr;
        if (!subscriber) return true;
        most += s.rate->limits().max;
        overused = overused || subscriber->backpressured || s.rate->overused(signal);
        return false;
    });
    if (streams.empty()) return;

    // And every path that carries copies goes through this node's uplink.
    const Copies all = copies();
    std::optional<uint64_t> uplink;
    bool found = false;
    {
        std::lock_guard lock(uplink_mutex_);
        found = uplink_.update(peer, signal, overused, all.paths.size(), now);
        uplink = uplink_.capacity();
    }
    const std::optional<double> per_most = all.per_most(uplink);
    if (found && per_most) limit_to_uplink(*per_most);
    pace_to_uplink(all, uplink);
    for (const auto& [id, stream] : streams) {
        std::optional<uint32_t> bitrate;
        {
            std::lock_guard lock(stream->mutex);
            OutgoingStream& s = *stream;
            // Gone since, or unsubscribed: the others' parts stay as they were.
            OutgoingStream::Subscriber* subscriber = s.rate ? s.subscriber_of(peer) : nullptr;
            if (!subscriber) continue;
            const double max = s.rate->limits().max;
            std::optional<uint64_t> part;
            if (per_most) part = static_cast<uint64_t>(*per_most * max);
            bitrate = s.follow(*subscriber, share_of(signal, max / most), part, now);
        }
        if (bitrate) notify(handlers_.bitrate_changed, id, *bitrate);
    }
}

void MediaSession::on_live_outcome(const PeerId& peer, uint64_t tag, bool delivered) {
    const auto slot = static_cast<uint16_t>(tag >> kSlotShift);
    const FrameNumber number = (tag & kNumberMask) - 1;
    StreamId id = 0;
    std::shared_ptr<OutgoingStream> stream;
    {
        std::shared_lock lock(mutex_);
        const auto it = std::ranges::find_if(
            outgoing_, [&](const auto& entry) { return entry.second->slot == slot; });
        if (it == outgoing_.end()) return;  // removed since
        id = it->first;
        stream = it->second;
    }

    bool ask = false;
    {
        std::lock_guard lock(stream->mutex);
        OutgoingStream& s = *stream;
        OutgoingStream::Subscriber* subscriber = s.subscriber_of(peer);
        if (!subscriber) return;
        // Sooner than the receiver could say: it would ask only once a frame
        // after it arrived, a round trip later.
        ask = s.settle(*subscriber, number, delivered) && ask_keyframe(s, clock_.now());
    }
    if (ask) notify(handlers_.keyframe_needed, id);
}

void MediaSession::on_message(const PeerId& peer, std::span<const uint8_t> message) {
    const Result<wire::MessageKind> kind = wire::peek_kind(message);
    if (!kind) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (*kind == wire::MessageKind::Frame) {
        on_frame(peer, message);
        return;
    }

    Result<wire::ControlMessage> control = wire::decode_control(message);
    if (!control) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::visit(
        [&](auto&& m) {
            using Message = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<Message, wire::Announce>) {
                on_announce(peer, std::move(m.description));
            } else if constexpr (std::is_same_v<Message, wire::Withdraw>) {
                on_withdraw(peer, m.stream);
            } else if constexpr (std::is_same_v<Message, wire::Subscribe>) {
                on_subscribe(peer, m.stream);
            } else if constexpr (std::is_same_v<Message, wire::Unsubscribe>) {
                on_unsubscribe(peer, m.stream);
            } else if constexpr (std::is_same_v<Message, wire::PresenceUpdate>) {
                on_presence(peer, std::move(m.presence));
            } else if constexpr (std::is_same_v<Message, wire::ClockProbe>) {
                // At once, on the clock the frames are captured on.
                network_.send_reliable(
                    peer, wire::encode_control(wire::ClockEcho{m.sent, capture_clock_.now()}));
            } else if constexpr (std::is_same_v<Message, wire::ClockEcho>) {
                on_clock_echo(peer, m.sent, m.at);
            } else if constexpr (std::is_same_v<Message, wire::PlayoutNeed>) {
                on_playout_need(peer, m.latency);
            } else if constexpr (std::is_same_v<Message, wire::PlayoutDelay>) {
                on_playout_delay(peer, m.delay);
            } else {
                static_assert(std::is_same_v<Message, wire::KeyframeRequest>);
                on_keyframe_request(peer, m.stream);
            }
        },
        std::move(*control));
}

void MediaSession::on_frame(const PeerId& peer, std::span<const uint8_t> message) {
    const Result<wire::FrameHeader> header = wire::read_frame_header(message);
    if (!header) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::shared_ptr<IncomingStream> stream = find_incoming(peer, header->info.stream);
    if (!stream) return;  // withdrawn while the frame was in flight

    const Timestamp now = clock_.now();
    std::lock_guard delivery(stream->delivery);
    {
        std::lock_guard lock(stream->mutex);
        if (!stream->subscribed) return;
        stream->bytes += message.size();
        const Buffer copy = Buffer::copy_of(message);
        stream->tracker.push(EncodedFrame{header->info, copy.slice(header->size)}, now);
    }
    deliver_ready(peer, header->info.stream, *stream, now);
}

// Hand on every frame the tracker has ready, then ask for a keyframe if the
// stream cannot be decoded. Called with `stream.delivery` held.
void MediaSession::deliver_ready(const PeerId& peer, StreamId id, IncomingStream& stream,
                                 Timestamp now) {
    for (;;) {
        std::optional<EncodedFrame> frame;
        bool want_keyframe = false;
        {
            std::lock_guard lock(stream.mutex);
            frame = stream.tracker.pop();
            if (!frame) want_keyframe = stream.subscribed && stream.tracker.want_keyframe(now);
        }
        if (!frame) {
            if (want_keyframe) {
                network_.send_reliable(peer, wire::encode_control(wire::KeyframeRequest{id}));
            }
            return;
        }
        notify(handlers_.frame_received, peer, std::move(*frame));
    }
}

void MediaSession::on_announce(const PeerId& peer, StreamDescription description) {
    const FrameTracker::Config config = tracker_config(description);
    std::shared_ptr<IncomingStream> stream;
    {
        std::unique_lock lock(mutex_);
        auto& slot = incoming_[StreamKey{peer, description.id}];
        if (!slot) {
            slot = std::make_shared<IncomingStream>(description, config);
            lock.unlock();
            notify(handlers_.stream_announced, peer, description);
            return;
        }
        stream = slot;
    }
    {
        std::lock_guard lock(stream->mutex);
        stream->description = description;
        stream->tracker.reconfigure(config);
    }
    notify(handlers_.stream_announced, peer, description);
}

void MediaSession::on_withdraw(const PeerId& peer, StreamId stream) {
    {
        std::unique_lock lock(mutex_);
        if (incoming_.erase(StreamKey{peer, stream}) == 0) return;
    }
    notify(handlers_.stream_withdrawn, peer, stream);
}

void MediaSession::on_subscribe(const PeerId& peer, StreamId stream_id) {
    const std::shared_ptr<OutgoingStream> stream = find_outgoing(stream_id);
    if (!stream) return;  // withdrawn while the request was in flight
    std::shared_ptr<PeerState> state = peer_state(peer);

    bool ask = false;
    {
        std::lock_guard lock(stream->mutex);
        const bool known = std::ranges::any_of(stream->subscribers, [&](const auto& subscriber) {
            return subscriber.peer->id == peer;
        });
        if (known) return;
        stream->subscribers.emplace_back(std::move(state));
    }
    // One more copy through an uplink known to be full: down to fit before
    // its keyframe is made, which would otherwise be the one to overfill it.
    if (stream->rate) {
        if (const std::optional<double> per_most = copies().per_most(uplink_capacity()))
            limit_to_uplink(*per_most);
    }
    {
        std::lock_guard lock(stream->mutex);
        ask = !stream->stands_alone() && stream->subscriber_of(peer) &&
              ask_keyframe(*stream, clock_.now());
    }
    if (ask) notify(handlers_.keyframe_needed, stream_id);
}

void MediaSession::pace_to_uplink(const Copies& all, std::optional<uint64_t> uplink) {
    // A ceiling is told again only when it moved by more than this: every path
    // samples ten times a second, and the uplink and its parts seldom change.
    constexpr double kWorthTelling = 0.05;
    std::vector<std::pair<PeerId, uint64_t>> told;
    {
        std::lock_guard lock(uplink_mutex_);
        for (auto it = ceilings_.begin(); it != ceilings_.end();) {
            if (uplink && all.paths.contains(it->first)) {
                ++it;
                continue;
            }
            told.emplace_back(it->first, 0);
            it = ceilings_.erase(it);
        }
        if (uplink && all.weight > 0) {
            for (const auto& [id, weight] : all.paths) {
                const auto ceiling = static_cast<uint64_t>(double(*uplink) * weight / all.weight);
                uint64_t& was = ceilings_[id];
                if (was != 0 &&
                    std::abs(double(ceiling) - double(was)) <= double(was) * kWorthTelling)
                    continue;
                was = ceiling;
                told.emplace_back(id, ceiling);
            }
        }
    }
    for (const auto& [id, ceiling] : told) network_.set_send_ceiling(id, ceiling);
}

void MediaSession::limit_to_uplink(double per_most) {
    std::vector<std::pair<StreamId, std::shared_ptr<OutgoingStream>>> streams;
    {
        std::shared_lock lock(mutex_);
        for (const auto& [id, stream] : outgoing_) streams.emplace_back(id, stream);
    }
    for (const auto& [id, stream] : streams) {
        std::optional<uint32_t> bitrate;
        {
            std::lock_guard lock(stream->mutex);
            OutgoingStream& s = *stream;
            if (!s.rate || s.subscribers.empty()) continue;
            const uint32_t before = s.rate->target();
            bitrate = s.count(s.rate->limit(static_cast<uint64_t>(per_most * s.rate->limits().max)),
                              before);
        }
        if (bitrate) notify(handlers_.bitrate_changed, id, *bitrate);
    }
}

void MediaSession::on_unsubscribe(const PeerId& peer, StreamId stream_id) {
    const std::shared_ptr<OutgoingStream> stream = find_outgoing(stream_id);
    if (!stream) return;
    std::optional<uint32_t> bitrate;
    {
        std::lock_guard lock(stream->mutex);
        const size_t erased = std::erase_if(stream->subscribers, [&](const auto& subscriber) {
            return subscriber.peer->id == peer;
        });
        if (erased > 0) bitrate = stream->restart_if_unwatched();
    }
    if (bitrate) notify(handlers_.bitrate_changed, stream_id, *bitrate);
}

void MediaSession::on_keyframe_request(const PeerId& peer, StreamId stream_id) {
    const std::shared_ptr<OutgoingStream> stream = find_outgoing(stream_id);
    if (!stream) return;

    const Timestamp now = clock_.now();
    bool ask = false;
    {
        std::lock_guard lock(stream->mutex);
        OutgoingStream& s = *stream;
        OutgoingStream::Subscriber* subscriber = s.subscriber_of(peer);
        if (!subscriber) return;
        s.stats.keyframe_requests += 1;
        if (s.stands_alone()) return;  // every frame is a keyframe
        // Its first keyframe is on its way, and the network will say if it is
        // not: the receiver asks only because it has nothing yet.
        if (subscriber->joining) return;
        // A request this soon after a keyframe was most likely sent before that
        // keyframe arrived. A receiver still unable to decode asks again after
        // keyframe_retry, by when this window has passed.
        if (s.last_keyframe_sent && now - *s.last_keyframe_sent < config_.min_keyframe_interval) {
            return;
        }
        ask = ask_keyframe(s, now);
    }
    if (ask) notify(handlers_.keyframe_needed, stream_id);
}

// ── Playing in step ─────────────────────────────────────────────────────────

void MediaSession::set_playout_need(const PeerId& peer, std::optional<Duration> need) {
    const std::shared_ptr<PeerState> state = find_peer(peer);
    if (!state) return;
    const Timestamp now = clock_.now();
    std::lock_guard lock(state->in_step_mutex);
    if (!need) {
        if (state->in_step && state->in_step->told) {
            network_.send_reliable(peer, wire::encode_control(wire::PlayoutNeed{}));
        }
        state->in_step.reset();
        return;
    }
    const bool starting = !state->in_step;
    if (starting) state->in_step.emplace();
    state->in_step->need = *need;
    if (starting) {
        probe_clock(*state, now);
    } else {
        tell_need(*state);
    }
}

std::optional<CommonPlayout> MediaSession::common_playout(const PeerId& peer) const {
    const std::shared_ptr<PeerState> state = find_peer(peer);
    if (!state) return std::nullopt;
    std::lock_guard lock(state->in_step_mutex);
    if (!state->in_step || !state->in_step->delay) return std::nullopt;
    const std::optional<Duration> offset = state->in_step->clock.offset();
    if (!offset) return std::nullopt;
    const Duration delay = *state->in_step->delay;
    return CommonPlayout{delay, delay + *offset};
}

void MediaSession::probe_clock(PeerState& state, Timestamp now) {
    PeerState::InStep& in_step = *state.in_step;
    network_.send_reliable(state.id, wire::encode_control(wire::ClockProbe{now}));
    ++in_step.probes;
    in_step.next_probe = now + (in_step.probes < kQuickProbes ? kQuickProbeEvery : kProbeEvery);
}

void MediaSession::on_clock_echo(const PeerId& peer, Timestamp sent, Timestamp at) {
    const Timestamp now = clock_.now();
    const std::shared_ptr<PeerState> state = find_peer(peer);
    if (!state) return;
    std::lock_guard lock(state->in_step_mutex);
    if (!state->in_step) return;  // no longer played in step
    state->in_step->clock.add(sent, at, now);
    tell_need(*state);
}

// What this node needs, on the peer's clock, once it is known: as it was
// set, or moved by more than kNeedNews since it was told.
void MediaSession::tell_need(PeerState& state) {
    PeerState::InStep& in_step = *state.in_step;
    const std::optional<Duration> offset = in_step.clock.offset();
    if (!offset) return;
    const Duration latency =
        std::clamp(in_step.need - *offset, Duration::zero(), wire::kMaxPlayout);
    if (in_step.told && std::chrono::abs(latency - *in_step.told) < kNeedNews) return;
    in_step.told = latency;
    network_.send_reliable(state.id, wire::encode_control(wire::PlayoutNeed{latency}));
}

void MediaSession::on_playout_need(const PeerId& peer, std::optional<Duration> latency) {
    if (!find_peer(peer)) return;  // gone while it was on its way
    std::lock_guard lock(viewers_mutex_);
    const bool joined = latency && !viewers_.plays(peer);
    if (viewers_.need(peer, latency, clock_.now())) {
        tell_delay(viewers_.viewers());
    } else if (joined && viewers_.delay()) {
        tell_delay({peer});
    }
}

void MediaSession::tell_delay(const std::vector<PeerId>& viewers) {
    const Buffer message = wire::encode_control(wire::PlayoutDelay{viewers_.delay()});
    for (const PeerId& viewer : viewers) network_.send_reliable(viewer, message);
}

void MediaSession::on_playout_delay(const PeerId& peer, std::optional<Duration> delay) {
    const std::shared_ptr<PeerState> state = find_peer(peer);
    if (!state) return;
    std::lock_guard lock(state->in_step_mutex);
    if (state->in_step) state->in_step->delay = delay;
}

// ── Timer ───────────────────────────────────────────────────────────────────

void MediaSession::tick() {
    const Timestamp now = clock_.now();

    std::vector<std::pair<StreamId, std::shared_ptr<OutgoingStream>>> outgoing;
    std::vector<std::pair<StreamKey, std::shared_ptr<IncomingStream>>> incoming;
    std::vector<std::shared_ptr<PeerState>> blocked;
    std::vector<std::shared_ptr<PeerState>> peers;
    {
        std::shared_lock lock(mutex_);
        for (const auto& [id, stream] : outgoing_) outgoing.emplace_back(id, stream);
        for (const auto& [key, stream] : incoming_) incoming.emplace_back(key, stream);
        peers.reserve(peers_.size());
        for (const auto& [id, state] : peers_) {
            if (state->blocked.load(std::memory_order_acquire)) blocked.push_back(state);
            peers.push_back(state);
        }
    }

    for (const auto& state : peers) {
        std::lock_guard lock(state->in_step_mutex);
        if (state->in_step && now >= state->in_step->next_probe) probe_clock(*state, now);
    }
    {
        std::lock_guard lock(viewers_mutex_);
        if (viewers_.update(now)) tell_delay(viewers_.viewers());
    }

    // A queue filled only by sends the network had not taken up yet never
    // raises the writable event when it drains, so blocked peers are polled.
    for (const auto& state : blocked) {
        if (network_.writable(state->id)) state->blocked.store(false, std::memory_order_release);
    }

    for (const auto& [id, stream] : outgoing) {
        bool ask = false;
        {
            std::lock_guard lock(stream->mutex);
            if (stream->keyframe_wanted) ask = ask_keyframe(*stream, now);
        }
        if (ask) notify(handlers_.keyframe_needed, id);
    }

    for (const auto& [key, stream] : incoming) {
        std::lock_guard delivery(stream->delivery);
        {
            std::lock_guard lock(stream->mutex);
            if (!stream->subscribed) continue;
            stream->tracker.update(now);
        }
        deliver_ready(key.peer, key.stream, *stream, now);
    }
}

uint64_t MediaSession::rejected_messages() const noexcept {
    return rejected_.load(std::memory_order_relaxed);
}

}  // namespace ratcast
