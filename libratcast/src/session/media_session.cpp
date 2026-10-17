#include "ratcast/session/media_session.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "wire/control_codec.h"
#include "wire/frame_codec.h"
#include "wire/message.h"

namespace ratcast {

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
    };

    OutgoingStream(StreamDescription d, std::optional<BitrateLimits> bitrate)
        : description(std::move(d)) {
        if (bitrate) rate.emplace(*bitrate, description.lifetime);
    }

    std::mutex mutex;  ///< guards everything below
    StreamDescription description;
    std::vector<Subscriber> subscribers;
    std::optional<FrameNumber> last_number;
    std::optional<Timestamp> last_keyframe_sent;
    std::optional<Timestamp> last_keyframe_asked;
    /// A keyframe was needed within min_keyframe_interval of the last one; tick()
    /// asks for it once the interval has passed, unless a keyframe came first.
    bool keyframe_wanted = false;
    std::optional<RateController> rate;  ///< for a rate-controlled stream
    OutgoingStreamStats stats;

    /// Counts a change rate control made and passes it on.
    std::optional<uint32_t> count(std::optional<uint32_t> bitrate, uint32_t before) {
        if (bitrate) (*bitrate < before ? stats.bitrate_cuts : stats.bitrate_raises) += 1;
        return bitrate;
    }

    /// Nobody watches any more: the next viewer starts afresh.
    std::optional<uint32_t> restart_if_unwatched() {
        if (!rate || !subscribers.empty()) return std::nullopt;
        const uint32_t before = rate->target();
        return count(rate->restart(), before);
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
    : network_(network), clock_(clock), handlers_(std::move(handlers)), config_(config) {}

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
        auto stream = std::make_shared<OutgoingStream>(description, bitrate);
        if (!outgoing_.try_emplace(description.id, std::move(stream)).second) {
            return fail(Errc::InvalidArgument);
        }
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
            subscriber.awaiting_keyframe = false;
            s.stats.messages += 1;
            s.stats.bytes += message->size();
            if (!network_.send_live(subscriber.peer->id, *message, s.description.lifetime)) {
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

// ── Network events ──────────────────────────────────────────────────────────

void MediaSession::on_peer_connected(const PeerId& peer) {
    std::vector<std::shared_ptr<OutgoingStream>> streams;
    {
        std::unique_lock lock(mutex_);
        auto& state = peers_[peer];
        if (!state) state = std::make_shared<PeerState>(peer);
        streams.reserve(outgoing_.size());
        for (const auto& [id, stream] : outgoing_) streams.push_back(stream);
    }
    for (const auto& stream : streams) {
        std::unique_lock lock(stream->mutex);
        wire::Announce announce{stream->description};
        lock.unlock();
        network_.send_reliable(peer, wire::encode_control(announce));
    }
}

void MediaSession::on_peer_disconnected(const PeerId& peer) {
    std::vector<std::pair<StreamId, std::shared_ptr<OutgoingStream>>> streams;
    std::vector<StreamId> withdrawn;
    {
        std::unique_lock lock(mutex_);
        peers_.erase(peer);
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
    PathSignal signal;
    {
        std::lock_guard lock(state->path_mutex);
        signal = state->path.update(sample, now);
    }

    for (const auto& [id, stream] : streams) {
        std::optional<uint32_t> bitrate;
        {
            std::lock_guard lock(stream->mutex);
            OutgoingStream& s = *stream;
            if (!s.rate) continue;
            const auto subscriber = std::ranges::find_if(
                s.subscribers, [&](const auto& subscriber) { return subscriber.peer->id == peer; });
            if (subscriber == s.subscribers.end()) continue;
            subscriber->capacity = signal.capacity;

            const uint32_t before = s.rate->target();
            if (std::exchange(subscriber->backpressured, false) || s.rate->overused(signal)) {
                bitrate = s.count(s.rate->on_overuse(now, signal.rtt, signal.capacity), before);
            } else {
                // Raised no further than the narrowest path is known to carry.
                std::optional<uint64_t> capacity;
                for (const auto& other : s.subscribers) {
                    if (other.capacity) {
                        capacity = std::min(capacity.value_or(*other.capacity), *other.capacity);
                    }
                }
                bitrate = s.count(s.rate->on_clear(now, capacity), before);
            }
        }
        if (bitrate) notify(handlers_.bitrate_changed, id, *bitrate);
    }
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
        ask = ask_keyframe(*stream, clock_.now());
    }
    if (ask) notify(handlers_.keyframe_needed, stream_id);
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
        const bool subscribed = std::ranges::any_of(
            s.subscribers, [&](const auto& subscriber) { return subscriber.peer->id == peer; });
        if (!subscribed) return;
        s.stats.keyframe_requests += 1;
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

// ── Timer ───────────────────────────────────────────────────────────────────

void MediaSession::tick() {
    const Timestamp now = clock_.now();

    std::vector<std::pair<StreamId, std::shared_ptr<OutgoingStream>>> outgoing;
    std::vector<std::pair<StreamKey, std::shared_ptr<IncomingStream>>> incoming;
    std::vector<std::shared_ptr<PeerState>> blocked;
    {
        std::shared_lock lock(mutex_);
        for (const auto& [id, stream] : outgoing_) outgoing.emplace_back(id, stream);
        for (const auto& [key, stream] : incoming_) incoming.emplace_back(key, stream);
        for (const auto& [id, state] : peers_) {
            if (state->blocked.load(std::memory_order_acquire)) blocked.push_back(state);
        }
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
