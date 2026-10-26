#include "ratcast/session/media_session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "session/sframe.h"
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

/// The copies of the streams through an uplink are short when each gets less
/// than this of the most its stream may take: the sender then hands viewers
/// to relays, until they are not or a single copy is left.
constexpr double kShort = 0.75;

/// Whether a frame that takes `reach` to a relay arrives in time at a viewer
/// `rtt` from that relay, with a repair on the way there (half the round trip,
/// then one more): within three quarters of the stream's `lifetime`, the rest
/// for queues and a keyframe's length.
bool in_time(Duration reach, Duration rtt, Duration lifetime) noexcept {
    return reach + rtt * 3 / 2 <= lifetime * 3 / 4;
}

/// A new key is sealed under this long after it was told: told reliably, it has
/// a few round trips to arrive before the first frame sealed under it.
constexpr Duration kRekeyAfter = 1s;
/// Keys of a sender's a viewer keeps: the one sealed under, the one told
/// next, and those before whose frames may still be on their way.
constexpr size_t kKeysKept = 4;

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
    /// The least of the last ten seconds, as of that sample: the path's own,
    /// without the queues along it.
    std::atomic<uint32_t> least_rtt_us{0};
    /// The peer said its uplink was found full (wire::Uplink): no relay to hand
    /// more viewers to.
    std::atomic<bool> uplink_full{false};
    /// The peer said it watches nothing and passes on what it is handed
    /// (wire::Forwarder): a server, the relay to hand viewers to first.
    std::atomic<bool> forwarder{false};
    bool is_forwarder() const noexcept { return forwarder.load(std::memory_order_relaxed); }

    Duration rtt() const noexcept {
        const uint32_t us = rtt_us.load(std::memory_order_relaxed);
        return us == 0 ? kUnknownRtt : Duration{us};
    }
    /// The least round trip of the last ten seconds: what a frame's time is
    /// judged by, a queue being rate control's to empty. None before the
    /// first sample.
    Duration least_rtt() const noexcept {
        return Duration{least_rtt_us.load(std::memory_order_relaxed)};
    }

    /// This node plays the peer's share in step (set_playout_need()).
    struct InStep {
        // Declared, so that std::optional sees it before PeerState is complete
        // (Clang judges a nested class's member initialisers only then).
        InStep() {}

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

namespace {

/// Where a frame goes among the live messages to a peer: sound first, then
/// a keyframe, then the frames that depend on it, and data with them.
LiveRank rank_of(MediaKind kind, const FrameInfo& info) noexcept {
    switch (kind) {
        case MediaKind::Audio: return LiveRank::Sound;
        case MediaKind::Data: return LiveRank::Frame;
        case MediaKind::Video: break;
    }
    return info.keyframe ? LiveRank::Keyframe : LiveRank::Frame;
}

}  // namespace

struct MediaSession::Copy {
    explicit Copy(std::shared_ptr<PeerState> state) : peer(std::move(state)) {}

    std::shared_ptr<PeerState> peer;
    /// Missed a frame (or has seen none yet): the frames before the next
    /// keyframe are of no use to it.
    bool awaiting_keyframe = true;
    /// A frame was held back for it since its path's last sample: its queue
    /// was full, which rate control takes for overuse.
    bool backpressured = false;
    /// What its path is known to carry, and carried over the last second, as
    /// of the last sample.
    std::optional<uint64_t> capacity;
    std::optional<uint64_t> carried;

    /// A keyframe of the stream is known to have reached it.
    bool joined = false;
    /// Its first keyframe, on its way: none before it is sent, nor once the
    /// network has said what became of it.
    std::optional<FrameNumber> joining;
    /// What the last frame to it was given, and when.
    Duration lifetime{};
    Timestamp sent_at{};

    /// At a stream's sender: the relay its frames go through, which passes
    /// them on to it; none: they are sent to it.
    std::shared_ptr<PeerState> relay;
    /// Relays it is not to go through: not connected to it, or too far for
    /// its frames to arrive in time (wire::CannotReach).
    std::vector<PeerId> not_through;
    /// Its relay's least round trip to it, as the relay last told
    /// (wire::RelayedPath).
    std::optional<Duration> relayed_rtt;

    /// Its frames are sent to it from here.
    bool direct() const noexcept { return relay == nullptr; }

    /// Its frames take another way: it starts over from the next keyframe, as
    /// one that never had any, and what the old way said no longer counts.
    void moved() noexcept {
        awaiting_keyframe = true;
        backpressured = false;
        capacity.reset();
        carried.reset();
        joined = false;
        joining.reset();
        relayed_rtt.reset();
    }
};

struct MediaSession::Fanout {
    std::vector<Copy> copies;
    /// The last keyframe sent.
    std::optional<FrameNumber> last_keyframe;
    /// Counts, as the outgoing stats name them.
    uint64_t messages = 0;
    uint64_t bytes = 0;
    uint64_t awaiting_keyframe = 0;
    uint64_t held_back = 0;

    Copy* find(const PeerId& peer) {
        const auto it =
            std::ranges::find_if(copies, [&](const Copy& copy) { return copy.peer->id == peer; });
        return it == copies.end() ? nullptr : &*it;
    }

    /// How long `copy` is given for a frame sent now that has `left` (its
    /// stream's lifetime at its sender, what is left of it at a relay). Its
    /// first keyframe gets kJoinRoundTrips round trips more; the frames after
    /// it get what the one before them got, less half the time since, so each
    /// is still due after the one before it.
    static Duration lifetime_for(Copy& copy, const FrameInfo& info, Duration left,
                                 bool stands_alone, Timestamp now) {
        if (stands_alone) return left;
        if (!copy.joined && info.keyframe && copy.awaiting_keyframe) {
            copy.joining = info.number;
            copy.lifetime = left + kJoinRoundTrips * copy.peer->rtt();
        } else {
            copy.lifetime = std::max(left, copy.lifetime - (now - copy.sent_at) / 2);
        }
        copy.sent_at = now;
        return copy.lifetime;
    }

    /// Send a frame that has `left` to every copy sent to from here that can
    /// use it, the message made by `encode()` at the first that can, once for
    /// all. Returns whether a copy waits for a keyframe; fails as `encode()`
    /// does.
    template <typename Encode>
    Result<bool> send(MediaNetwork& network, const FrameInfo& info, MediaKind kind, Duration left,
                      uint64_t tag, Timestamp now, Encode&& encode) {
        const bool stands_alone = kind != MediaKind::Video;
        if (info.keyframe) last_keyframe = std::max(last_keyframe.value_or(0), info.number);
        std::optional<Buffer> message;
        bool want_keyframe = false;
        for (Copy& copy : copies) {
            if (!copy.direct()) continue;
            const bool useful = info.keyframe || !copy.awaiting_keyframe;
            if (copy.peer->blocked.load(std::memory_order_acquire)) {
                // Everything after this frame may reference it, so the copy can
                // only pick up again at a keyframe. Not asked for yet: it could
                // not be sent either, and keyframes are the last thing a full
                // queue needs. The first frame after it drains asks.
                if (useful) {
                    copy.awaiting_keyframe = true;
                    copy.backpressured = true;
                    held_back += 1;
                } else {
                    awaiting_keyframe += 1;
                }
                continue;
            }
            if (!useful) {
                awaiting_keyframe += 1;
                want_keyframe = true;
                continue;
            }

            if (!message) {
                Result<Buffer> encoded = encode();
                if (!encoded) return fail(encoded.error());
                message = std::move(*encoded);
            }
            const Duration lifetime = lifetime_for(copy, info, left, stands_alone, now);
            copy.awaiting_keyframe = false;
            messages += 1;
            bytes += message->size();
            if (!network.send_live(copy.peer->id, *message, lifetime, tag, rank_of(kind, info))) {
                copy.peer->blocked.store(true, std::memory_order_release);
                copy.backpressured = true;
            }
        }
        return want_keyframe;
    }

    /// A frame that can no longer arrive in time is not sent: the copies that
    /// could have used it wait for a keyframe. Returns whether any does.
    bool miss(const FrameInfo& info, bool stands_alone) {
        if (stands_alone) return false;
        bool missed = false;
        for (Copy& copy : copies) {
            if (!copy.direct() || (!info.keyframe && copy.awaiting_keyframe)) continue;
            copy.awaiting_keyframe = true;
            held_back += 1;
            missed = true;
        }
        return missed;
    }

    /// The network settled frame `number` to `copy`. Returns whether that
    /// leaves it needing a keyframe.
    bool settle(Copy& copy, FrameNumber number, bool delivered) {
        if (copy.joining && number == *copy.joining) {
            copy.joining.reset();
            if (delivered) {
                copy.joined = true;
                return false;
            }
            copy.awaiting_keyframe = true;  // what follows it is of no use
            return true;
        }
        // What follows a frame given up on cannot be decoded, up to a keyframe
        // sent after it. Behind a joining keyframe still on its way too: asked
        // for now, the next one goes right behind it, as late as it may be.
        return !delivered && (copy.joined || copy.joining.has_value()) &&
               !(last_keyframe && *last_keyframe > number);
    }
};

struct MediaSession::OutgoingStream {
    OutgoingStream(StreamDescription d, std::optional<BitrateLimits> bitrate, uint16_t tag_slot)
        : slot(tag_slot), description(std::move(d)) {
        if (bitrate) rate.emplace(*bitrate, description.lifetime);
    }

    /// Every frame of it decodes on its own — sound, or bytes ratcast does not
    /// look into: nothing waits for a keyframe, and no frame is worth more
    /// time than the others.
    bool stands_alone() const noexcept { return description.kind() != MediaKind::Video; }

    const uint16_t slot;  ///< in the tags of its frames
    std::mutex mutex;     ///< guards everything below
    StreamDescription description;
    /// Its subscribers, those its frames go to through a relay among them.
    Fanout fanout;
    std::optional<FrameNumber> last_number;
    std::optional<Timestamp> last_keyframe_sent;
    std::optional<Timestamp> last_keyframe_asked;
    /// A keyframe was needed within min_keyframe_interval of the last one; tick()
    /// asks for it once the interval has passed, unless a keyframe came first.
    bool keyframe_wanted = false;
    std::optional<RateController> rate;  ///< for a rate-controlled stream
    OutgoingStreamStats stats;

    Copy* subscriber_of(const PeerId& peer) { return fanout.find(peer); }

    /// The subscriber its frames go to straight from here, as `peer`.
    Copy* direct_subscriber_of(const PeerId& peer) {
        Copy* copy = fanout.find(peer);
        return copy && copy->direct() ? copy : nullptr;
    }

    /// The most bitrate it may take, as a relay is told; 0 without rate control.
    uint32_t most() const noexcept { return rate ? rate->limits().max : 0; }

    /// Whether its subscribers may get it through relays: pictures whose rate
    /// follows the paths. Sound stays direct: small, and what pictures are
    /// held to.
    bool relayable() const noexcept { return rate && !stands_alone(); }

    /// How many relays `copy`'s frames go through.
    size_t hops(const Copy& copy) {
        size_t n = 0;
        for (const Copy* c = &copy; c && c->relay && n < fanout.copies.size();
             c = subscriber_of(c->relay->id)) {
            ++n;
        }
        return n;
    }

    /// How many subscribers `relay` passes it on to.
    size_t passed_on_by(const PeerState& relay) const {
        return static_cast<size_t>(std::ranges::count_if(
            fanout.copies, [&](const Copy& copy) { return copy.relay.get() == &relay; }));
    }

    /// The last viewer to come that passes it on to nobody, of those that
    /// `relay` passes it on to (null: sent to from here). A forwarder is no
    /// viewer.
    Copy* last_leaf(const PeerState* relay) {
        for (auto it = fanout.copies.rbegin(); it != fanout.copies.rend(); ++it) {
            if (it->relay.get() == relay && !it->peer->is_forwarder() &&
                passed_on_by(*it->peer) == 0)
                return &*it;
        }
        return nullptr;
    }

    /// Copies sent to from here.
    size_t direct_copies() const {
        return static_cast<size_t>(std::ranges::count_if(fanout.copies, &Copy::direct));
    }

    /// The least the round trip between two subscribers can be: the
    /// difference of theirs from here.
    static Duration between(const Copy& one, const Copy& other) {
        return std::chrono::abs(one.peer->least_rtt() - other.peer->least_rtt());
    }

    /// How long a frame takes from here to `copy`, as far as is known: half
    /// the round trip to the first relay on its way, then half of each relay's
    /// to the next, as the relay told — or, until it has, at least half the
    /// difference of their round trips from here.
    Duration reach(const Copy& copy) {
        Duration total{};
        const Copy* c = &copy;
        for (size_t n = 0; c->relay && n < fanout.copies.size(); ++n) {
            const Copy* relay = subscriber_of(c->relay->id);
            if (!relay) break;
            total += c->relayed_rtt.value_or(between(*c, *relay)) / 2;
            c = relay;
        }
        return total + c->peer->least_rtt() / 2;
    }

    /// Whether `relay` may pass it on to `viewer` at all: not known to be full
    /// nor unable to, as far as is known.
    static bool may_pass_on(const Copy& viewer, const PeerState& relay) {
        return relay.id != viewer.peer->id && !relay.uplink_full.load(std::memory_order_relaxed) &&
               std::ranges::find(viewer.not_through, relay.id) == viewer.not_through.end();
    }

    /// The relay best placed to pass it on to `viewer`, which passes it on to
    /// nobody: one connected to it whose uplink is not known to be full,
    /// through which its frames may arrive in time (in_time(), the round trip
    /// between them at least the difference of theirs from here). A forwarder
    /// first — one sent to already, else one of `forwarders` made a
    /// subscriber now, if `new_forwarder` — then the subscriber the fewest hops
    /// from here, passing it on to the fewest, the nearest (the round trip from
    /// here: what is known of where it is). With `carried` — a viewer a server
    /// carries anyway, its frames through the server however they are sent —
    /// a forwarder or none, in time or not: the way is no longer for it.
    std::shared_ptr<PeerState> relay_for(const Copy& viewer, const Forwarders& forwarders,
                                         bool new_forwarder, bool carried = false) {
        const auto timely = [&](Duration reach, Duration rtt) {
            return carried || in_time(reach, rtt, description.lifetime);
        };
        const Copy* best = nullptr;
        std::tuple<bool, size_t, size_t, Duration> best_by{};
        for (const Copy& copy : fanout.copies) {
            if (!may_pass_on(viewer, *copy.peer) || (carried && !copy.peer->is_forwarder()) ||
                !timely(reach(copy), between(viewer, copy))) {
                continue;
            }
            const std::tuple by{!copy.peer->is_forwarder(), hops(copy), passed_on_by(*copy.peer),
                                copy.peer->rtt()};
            if (!best || by < best_by) {
                best = &copy;
                best_by = by;
            }
        }
        if (best && best->peer->is_forwarder()) return best->peer;
        if (new_forwarder) {
            for (const std::shared_ptr<PeerState>& forwarder : forwarders) {
                const Duration apart =
                    std::chrono::abs(viewer.peer->least_rtt() - forwarder->least_rtt());
                if (subscriber_of(forwarder->id) || !may_pass_on(viewer, *forwarder) ||
                    !timely(forwarder->least_rtt() / 2, apart)) {
                    continue;
                }
                // Sent to from now on, from the next keyframe, as a subscriber.
                fanout.copies.emplace_back(forwarder);
                return forwarder;
            }
        }
        return best && !carried ? best->peer : nullptr;
    }

    /// `viewer`'s frames go through `relay` from now on (null: from here);
    /// the relays it leaves and joins are told, in `told`. Returns whether it
    /// is sent to from here again, and so needs a keyframe.
    /// Every copy may be gone or moved in the vector by then: a forwarder
    /// left passing it on to nobody is dropped.
    bool move(Copy& viewer, std::shared_ptr<PeerState> relay,
              std::vector<std::pair<PeerId, Buffer>>& told) {
        if (viewer.relay == relay) return false;
        const std::shared_ptr<PeerState> was = std::exchange(viewer.relay, std::move(relay));
        viewer.moved();
        const bool direct = viewer.direct();
        if (was) told.emplace_back(was->id, forward_message(*was));
        if (viewer.relay) told.emplace_back(viewer.relay->id, forward_message(*viewer.relay));
        drop_idle_forwarders();
        return direct;
    }

    /// A forwarder passing it on to nobody is no longer sent to: it was told
    /// so (an empty Forward), and lets go of the stream.
    void drop_idle_forwarders() {
        std::erase_if(fanout.copies, [&](const Copy& copy) {
            return copy.peer->is_forwarder() && passed_on_by(*copy.peer) == 0;
        });
    }

    /// Takes a copy off this node's uplink: the last viewer to come of those
    /// sent to from here and passing it on to nobody goes through the relay
    /// best placed (relay_for()) — a forwarder not yet sent to only where
    /// another copy is sent from here, which it then takes off too. Returns
    /// the copies taken off: 1, or 0 for a forwarder made a subscriber; none
    /// where there was no viewer to move or no relay to take it.
    std::optional<int> hand_off(const Forwarders& forwarders,
                                std::vector<std::pair<PeerId, Buffer>>& told) {
        const Copy* viewer = last_leaf(nullptr);
        if (!viewer) return std::nullopt;
        const PeerId id = viewer->peer->id;
        const size_t before = fanout.copies.size();
        std::shared_ptr<PeerState> relay = relay_for(*viewer, forwarders, direct_copies() >= 2);
        if (!relay) return std::nullopt;
        const int taken = fanout.copies.size() > before ? 0 : 1;
        move(*subscriber_of(id), std::move(relay), told);  // the vector may have grown
        return taken;
    }

    /// What `relay` is to pass on, all of it.
    Buffer forward_message(const PeerState& relay) const {
        wire::Forward forward{description.id, most(), {}};
        for (const Copy& copy : fanout.copies) {
            if (copy.relay.get() == &relay) forward.viewers.push_back(copy.peer->id);
        }
        return wire::encode_control(forward);
    }

    /// Counts a change rate control made and passes it on.
    std::optional<uint32_t> count(std::optional<uint32_t> bitrate, uint32_t before) {
        if (bitrate) (*bitrate < before ? stats.bitrate_cuts : stats.bitrate_raises) += 1;
        return bitrate;
    }

    /// Rate control hears a sample of `subscriber`'s path — from here, or as
    /// its relay told — of which a copy may take at most `uplink` where the
    /// uplink they all go through is known.
    std::optional<uint32_t> follow(Copy& subscriber, PathSignal path,
                                   std::optional<uint64_t> uplink, Timestamp now) {
        if (uplink) path.capacity = std::min(path.capacity.value_or(*uplink), *uplink);
        subscriber.capacity = path.capacity;
        subscriber.carried = path.carried;
        const uint32_t before = rate->target();
        if (std::exchange(subscriber.backpressured, false) || rate->overused(path)) {
            return count(rate->on_overuse(now, path), before);
        }
        // Raised no further than the narrowest path is known to carry; past
        // where one filled up, by more than a step once it carried more.
        const auto least = [](std::optional<uint64_t> a, std::optional<uint64_t> b) {
            return a && b ? std::min(a, b) : a ? a : b;
        };
        std::optional<uint64_t> capacity;
        std::optional<uint64_t> carried;
        for (const Copy& other : fanout.copies) {
            capacity = least(capacity, other.capacity);
            carried = least(carried, other.carried);
        }
        return count(rate->on_clear(now, capacity, carried), before);
    }

    /// Nobody watches any more: the next viewer starts afresh. Neither a cut
    /// nor a raise: no path asked for it.
    std::optional<uint32_t> restart_if_unwatched() {
        if (!rate || !fanout.copies.empty()) return std::nullopt;
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
    /// Its slot in the tags of the frames this node passes on; 0 until it
    /// first does. Set under the session's mutex_.
    std::atomic<uint16_t> relay_slot{0};
    std::mutex mutex;  ///< guards everything below
    StreamDescription description;
    bool subscribed = false;
    FrameTracker tracker;
    uint64_t bytes = 0;
    uint64_t opened = 0;    ///< frames sealed, and opened
    uint64_t unopened = 0;  ///< frames dropped unread

    /// This node passes the stream on to the viewers its sender handed it
    /// (Forward): the copies of `relayed`, none while it passes it on to nobody.
    Fanout relayed;
    /// The most the stream may take, as its sender said; 0: not rate-controlled.
    uint32_t relayed_most = 0;
    std::optional<Timestamp> keyframe_asked;  ///< of the sender, for those it passes it on to
};

struct MediaSession::Sealing {
    std::mutex mutex;  ///< guards everything below

    // This node's: the key it seals under, the one it is to from `from` on.
    struct Told {
        sframe::BaseKey base;
        sframe::Key key;
        Timestamp from{};
    };
    std::optional<Told> current;
    std::optional<Told> next;
    uint64_t next_kid = 0;
    uint64_t ctr = 0;  ///< the next frame's, under the current key
    /// The peers given the key, and so each new one.
    std::unordered_set<PeerId, PeerId::Hash> keyed;

    /// The keys peers gave this node, by peer, the newest last.
    std::unordered_map<PeerId, std::deque<sframe::Key>, PeerId::Hash> given;

    Told make(Timestamp from) {
        const sframe::BaseKey base = sframe::random_key();
        return Told{base, sframe::Key(next_kid++, base), from};
    }

    /// What tells a peer the keys: the one sealed under, and the next.
    std::vector<Buffer> messages() const {
        std::vector<Buffer> out;
        for (const std::optional<Told>* told : {&current, &next}) {
            if (*told) {
                out.push_back(
                    wire::encode_control(wire::FrameKey{(*told)->key.kid(), (*told)->base}));
            }
        }
        return out;
    }

    /// The key to seal a frame under now, and its CTR.
    std::pair<sframe::Key, uint64_t> take(Timestamp now) {
        std::lock_guard lock(mutex);
        if (next && now >= next->from) {
            current = std::move(next);
            next.reset();
            ctr = 0;
        }
        return {current->key, ctr++};
    }

    void keep(const PeerId& peer, uint64_t kid, const sframe::BaseKey& base) {
        std::lock_guard lock(mutex);
        std::deque<sframe::Key>& keys = given[peer];
        if (std::ranges::any_of(keys, [&](const sframe::Key& key) { return key.kid() == kid; }))
            return;
        keys.emplace_back(kid, base);
        if (keys.size() > kKeysKept) keys.pop_front();
    }

    std::optional<sframe::Key> of(const PeerId& peer, uint64_t kid) {
        std::lock_guard lock(mutex);
        const auto keys = given.find(peer);
        if (keys == given.end()) return std::nullopt;
        for (const sframe::Key& key : keys->second) {
            if (key.kid() == kid) return key;
        }
        return std::nullopt;
    }
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
      chat_(config.chat_kept),
      capture_clock_(config.capture_clock ? *config.capture_clock : clock),
      sealing_(std::make_unique<Sealing>()) {
    if (config_.seal) sealing_->current = sealing_->make(clock_.now());
}

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

std::vector<std::pair<StreamId, std::shared_ptr<MediaSession::OutgoingStream>>>
MediaSession::outgoing_streams() const {
    std::shared_lock lock(mutex_);
    return {outgoing_.begin(), outgoing_.end()};
}

uint16_t MediaSession::take_slot() {
    const uint16_t slot = next_slot_;
    if (++next_slot_ == 0) next_slot_ = 1;
    return slot;
}

MediaSession::Forwarders MediaSession::forwarders() const {
    std::shared_lock lock(mutex_);
    Forwarders forwarders;
    for (const auto& [id, state] : peers_) {
        if (state->is_forwarder()) forwarders.push_back(state);
    }
    return forwarders;
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
                          std::make_shared<OutgoingStream>(description, bitrate, take_slot()));
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
    // With room for a relay to pass it on, sealed.
    if (wire::frame_header_size(info) + payload.size() + wire::kRelayedFrameOverhead +
            (config_.seal ? sframe::kMaxOverhead : 0) >
        network_.max_live_message()) {
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
        // What became of a frame that stands alone changes nothing.
        const uint64_t tag = s.stands_alone() ? 0 : live_tag(s.slot, info.number);
        // Encoded on first use — sealed behind its header, which is left to
        // read — once for every subscriber, never when none can use it.
        const Result<bool> want_keyframe = s.fanout.send(
            network_, info, s.description.kind(), s.description.lifetime, tag, now,
            [&]() -> Result<Buffer> {
                if (!config_.seal) return wire::encode_frame(info, std::move(payload));
                std::array<uint8_t, wire::kMaxFrameHeaderSize> header;
                const std::span metadata(header.data(), wire::frame_header_size(info));
                wire::write_frame_header(info, /*sealed=*/true, metadata);
                const auto [key, ctr] = sealing_->take(now);
                return wire::encode_frame(info, key.seal(ctr, metadata, payload.bytes()),
                                          /*sealed=*/true);
            });
        if (!want_keyframe) return fail(want_keyframe.error());
        ask = *want_keyframe && ask_keyframe(s, now);
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
    std::vector<std::shared_ptr<IncomingStream>> relayed;
    {
        std::shared_lock lock(mutex_);
        for (const auto& [id, stream] : outgoing_) streams.push_back(stream);
        for (const auto& [key, stream] : incoming_) {
            if (stream->relay_slot.load(std::memory_order_relaxed) != 0) relayed.push_back(stream);
        }
    }
    Copies copies;
    const auto add = [&](const Fanout& fanout, double most) {
        for (const Copy& copy : fanout.copies) {
            if (!copy.direct()) continue;  // through another's uplink
            copies.weight += most;
            copies.paths[copy.peer->id] += most;
        }
    };
    for (const auto& stream : streams) {
        std::lock_guard lock(stream->mutex);
        if (stream->rate) add(stream->fanout, stream->rate->limits().max);
    }
    for (const auto& stream : relayed) {
        std::lock_guard lock(stream->mutex);
        add(stream->relayed, stream->relayed_most);
    }
    return copies;
}

std::optional<double> MediaSession::Copies::per_most(std::optional<uint64_t> uplink) const {
    if (!uplink || weight <= 0 || paths.size() < 2) return std::nullopt;
    return double(*uplink) * SharedUplink::kCopies / weight;
}

std::optional<OutgoingStreamStats> MediaSession::outgoing_stats(StreamId stream_id) const {
    const std::shared_ptr<OutgoingStream> stream = find_outgoing(stream_id);
    if (!stream) return std::nullopt;
    std::lock_guard lock(stream->mutex);
    OutgoingStreamStats stats = stream->stats;
    const Fanout& fanout = stream->fanout;
    stats.subscribers = static_cast<size_t>(std::ranges::count_if(
        fanout.copies, [](const Copy& copy) { return !copy.peer->is_forwarder(); }));
    stats.relayed = static_cast<size_t>(
        std::ranges::count_if(fanout.copies, [](const Copy& copy) { return !copy.direct(); }));
    stats.messages = fanout.messages;
    stats.bytes = fanout.bytes;
    stats.awaiting_keyframe = fanout.awaiting_keyframe;
    stats.backpressure_drops = fanout.held_back;
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
        stream->relayed.copies.clear();       // its sender hands them on, hearing of it
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
    const Fanout& relayed = stream->relayed;
    return IncomingStreamStats{
        .subscribed = stream->subscribed,
        .bytes = stream->bytes,
        .frames = stream->tracker.stats(),
        .relaying_to = relayed.copies.size(),
        .relayed_messages = relayed.messages,
        .relayed_bytes = relayed.bytes,
        .relay_held_back = relayed.held_back,
        .opened = stream->opened,
        .unopened = stream->unopened,
    };
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
    give_key(peer, /*again=*/true);
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
    // What the peer passes on of this node's streams.
    if (const std::shared_ptr<PeerState> relay = find_peer(peer)) {
        for (const auto& [id, stream] : outgoing_streams()) {
            std::unique_lock lock(stream->mutex);
            const auto& copies = stream->fanout.copies;
            if (std::ranges::none_of(copies, [&](const Copy& c) { return c.relay == relay; })) {
                continue;
            }
            const Buffer forward = stream->forward_message(*relay);
            lock.unlock();
            network_.send_reliable(peer, forward);
        }
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
    if (const std::optional<uint64_t> uplink = uplink_capacity()) {
        network_.send_reliable(peer, wire::encode_control(wire::Uplink{uplink}));
    }
    if (config_.forwarder) network_.send_reliable(peer, wire::encode_control(wire::Forwarder{}));
    {
        std::lock_guard lock(presence_mutex_);
        if (presence_) {
            network_.send_reliable(peer, wire::encode_control(wire::PresenceUpdate{*presence_}));
        }
    }
    // Asked of a server too, which does not answer: who is one may not be
    // known yet.
    if (!config_.forwarder) network_.send_reliable(peer, wire::encode_control(wire::ChatAsk{}));
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

bool MediaSession::entitled(const PeerId& peer) const {
    const std::shared_ptr<PeerState> state = find_peer(peer);
    if (state && state->is_forwarder()) return false;
    return !config_.entitled || config_.entitled(peer);
}

// ── What is said ────────────────────────────────────────────────────────────

Result<ChatMessage> MediaSession::say(std::string text, WallTime now) {
    if (config_.forwarder) return fail(Errc::Unsupported);
    ChatMessage message{.author = network_.local_id(), .text = std::move(text)};
    if (message.author.is_zero()) return fail(Errc::Unavailable);
    {
        std::lock_guard lock(presence_mutex_);
        if (presence_) message.name = presence_->name;
    }
    // Stamped, kept and sent under the lock: two said at once go in the order
    // they were said.
    std::lock_guard lock(chat_mutex_);
    message.sent = std::max(now, last_said_ + WallTime::duration{1});
    if (!is_valid(message)) return fail(Errc::InvalidArgument);
    last_said_ = message.sent;
    chat_.add(message);
    const Buffer said = wire::encode_control(wire::Chat{message.sent, message.text});
    for (const PeerId& peer : connected_peers()) {
        if (entitled(peer)) network_.send_reliable(peer, said);
    }
    return message;
}

std::vector<ChatMessage> MediaSession::chat() const {
    std::lock_guard lock(chat_mutex_);
    return {chat_.messages().begin(), chat_.messages().end()};
}

void MediaSession::on_chat(const PeerId& peer, ChatMessage message, bool told) {
    if (config_.forwarder || !find_peer(peer) || !entitled(peer)) return;
    if (!told) {
        message.author = peer;
        if (std::optional<Presence> said = presence(peer)) message.name = std::move(said->name);
    }
    {
        std::lock_guard lock(chat_mutex_);
        if (!chat_.add(message)) return;
    }
    notify(handlers_.said, message);
}

void MediaSession::tell_chat(const PeerId& peer) {
    if (config_.forwarder || !entitled(peer)) return;
    std::vector<Buffer> told;
    {
        std::lock_guard lock(chat_mutex_);
        told.reserve(chat_.messages().size());
        for (const ChatMessage& message : chat_.messages()) {
            told.push_back(wire::encode_control(wire::ChatTold{message}));
        }
    }
    for (const Buffer& message : told) network_.send_reliable(peer, message);
}

void MediaSession::on_peer_disconnected(const PeerId& peer) {
    std::vector<std::pair<StreamId, std::shared_ptr<OutgoingStream>>> streams;
    std::vector<std::shared_ptr<IncomingStream>> relayed;
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
                if (it->second->relay_slot.load(std::memory_order_relaxed) != 0)
                    relayed.push_back(it->second);
                ++it;
            }
        }
        for (const auto& [id, stream] : outgoing_) streams.emplace_back(id, stream);
    }
    // Nothing more to pass on to it; its sender hands it on, hearing of it.
    for (const auto& stream : relayed) {
        std::lock_guard lock(stream->mutex);
        std::erase_if(stream->relayed.copies,
                      [&](const Copy& copy) { return copy.peer->id == peer; });
    }
    {
        std::lock_guard lock(uplink_mutex_);
        uplink_.forget(peer);
        ceilings_.erase(peer);  // the network forgot it too
    }
    rekey_without(peer, clock_.now());
    {
        std::lock_guard lock(viewers_mutex_);
        if (viewers_.need(peer, std::nullopt, clock_.now())) tell_delay(viewers_.viewers());
    }
    for (const auto& [id, stream] : streams) drop_subscriber(id, *stream, peer);
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
    std::vector<std::pair<StreamKey, std::shared_ptr<IncomingStream>>> relayed;
    {
        std::shared_lock lock(mutex_);
        const auto it = peers_.find(peer);
        if (it == peers_.end()) return;
        state = it->second;
        for (const auto& [id, stream] : outgoing_) streams.emplace_back(id, stream);
        for (const auto& [key, stream] : incoming_) {
            if (stream->relay_slot.load(std::memory_order_relaxed) != 0)
                relayed.emplace_back(key, stream);
        }
    }
    state->rtt_us.store(sample.srtt_us, std::memory_order_relaxed);
    PathSignal signal;
    {
        std::lock_guard lock(state->path_mutex);
        signal = state->path.update(sample, now);
    }
    state->least_rtt_us.store(static_cast<uint32_t>((signal.rtt - signal.queue_rise).count()),
                              std::memory_order_relaxed);

    // The rate-controlled streams sent to the peer share its path, each in
    // proportion to the most it may take: this node's own, and those it
    // passes on.
    double most = 0;
    bool overused = false;
    std::erase_if(streams, [&](const auto& entry) {
        std::lock_guard lock(entry.second->mutex);
        OutgoingStream& s = *entry.second;
        const Copy* subscriber = s.rate ? s.direct_subscriber_of(peer) : nullptr;
        if (!subscriber) return true;
        most += s.rate->limits().max;
        overused = overused || subscriber->backpressured || s.rate->overused(signal);
        return false;
    });
    std::erase_if(relayed, [&](const auto& entry) {
        std::lock_guard lock(entry.second->mutex);
        IncomingStream& s = *entry.second;
        const Copy* copy = s.relayed_most > 0 ? s.relayed.find(peer) : nullptr;
        if (!copy) return true;
        most += s.relayed_most;
        overused = overused || copy->backpressured || overuses(signal, s.description.lifetime);
        return false;
    });
    if (streams.empty() && relayed.empty()) return;

    // And every path that carries copies goes through this node's uplink.
    Copies all = copies();
    std::optional<uint64_t> uplink;
    bool found = false;
    bool news = false;
    {
        std::lock_guard lock(uplink_mutex_);
        const std::optional<uint64_t> before = uplink_.capacity();
        found = uplink_.update(peer, signal, overused, all.paths.size(), now);
        uplink = uplink_.capacity();
        news = found || (before && !uplink);
    }
    // The peers choosing relays hear of it; and copies through it go through
    // relays where they can.
    if (news) tell_uplink(uplink);
    if (found) {
        arrange_all();
        all = copies();
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
            Copy* subscriber = s.rate ? s.direct_subscriber_of(peer) : nullptr;
            if (!subscriber) continue;
            const double max = s.rate->limits().max;
            std::optional<uint64_t> part;
            if (per_most) part = static_cast<uint64_t>(*per_most * max);
            bitrate = s.follow(*subscriber, share_of(signal, max / most), part, now);
        }
        if (bitrate) notify(handlers_.bitrate_changed, id, *bitrate);
    }
    // What this node passes on is rate-controlled where it is sent from: its
    // sender hears what the path says, of the stream's part of it.
    for (const auto& [key, stream] : relayed) {
        wire::RelayedPath report{key.stream, peer, {}, false};
        {
            std::lock_guard lock(stream->mutex);
            IncomingStream& s = *stream;
            Copy* copy = s.relayed.find(peer);
            if (!copy) continue;
            report.signal = share_of(signal, s.relayed_most / most);
            report.backpressured = std::exchange(copy->backpressured, false);
            if (per_most) {
                const auto part = static_cast<uint64_t>(*per_most * s.relayed_most);
                report.signal.capacity = std::min(report.signal.capacity.value_or(part), part);
            }
        }
        network_.send_reliable(key.peer, wire::encode_control(report));
    }
}

void MediaSession::on_live_outcome(const PeerId& peer, uint64_t tag, bool delivered) {
    const auto slot = static_cast<uint16_t>(tag >> kSlotShift);
    const FrameNumber number = (tag & kNumberMask) - 1;
    StreamId id = 0;
    std::shared_ptr<OutgoingStream> stream;
    std::shared_ptr<IncomingStream> relayed;
    PeerId origin;
    {
        std::shared_lock lock(mutex_);
        const auto it = std::ranges::find_if(
            outgoing_, [&](const auto& entry) { return entry.second->slot == slot; });
        if (it != outgoing_.end()) {
            id = it->first;
            stream = it->second;
        } else {
            const auto passed = std::ranges::find_if(incoming_, [&](const auto& entry) {
                return entry.second->relay_slot.load(std::memory_order_relaxed) == slot;
            });
            if (passed == incoming_.end()) return;  // gone since
            origin = passed->first.peer;
            id = passed->first.stream;
            relayed = passed->second;
        }
    }
    if (relayed) {
        std::lock_guard lock(relayed->mutex);
        Copy* copy = relayed->relayed.find(peer);
        if (copy && relayed->relayed.settle(*copy, number, delivered)) {
            ask_keyframe_for_relayed(origin, id, *relayed, clock_.now());
        }
        return;
    }

    bool ask = false;
    {
        std::lock_guard lock(stream->mutex);
        OutgoingStream& s = *stream;
        Copy* subscriber = s.subscriber_of(peer);
        if (!subscriber) return;
        // Sooner than the receiver could say: it would ask only once a frame
        // after it arrived, a round trip later.
        ask = s.fanout.settle(*subscriber, number, delivered) && ask_keyframe(s, clock_.now());
    }
    if (ask) notify(handlers_.keyframe_needed, id);
}

void MediaSession::on_message(const PeerId& peer, std::span<const uint8_t> message,
                              std::optional<Timestamp> deadline) {
    const Result<wire::MessageKind> kind = wire::peek_kind(message);
    if (!kind) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (*kind == wire::MessageKind::Frame) {
        on_frame(peer, peer, message, deadline);
        return;
    }
    if (*kind == wire::MessageKind::RelayedFrame) {
        const Result<wire::RelayedFrame> relayed = wire::read_relayed_frame(message);
        if (!relayed) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        on_frame(relayed->origin, peer, relayed->frame, deadline);
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
            } else if constexpr (std::is_same_v<Message, wire::Forward>) {
                on_forward(peer, m);
            } else if constexpr (std::is_same_v<Message, wire::RelayedPath>) {
                on_relayed_path(peer, m);
            } else if constexpr (std::is_same_v<Message, wire::Uplink>) {
                on_uplink(peer, m.capacity);
            } else if constexpr (std::is_same_v<Message, wire::CannotReach>) {
                on_cannot_reach(peer, m);
            } else if constexpr (std::is_same_v<Message, wire::Forwarder>) {
                on_forwarder(peer);
            } else if constexpr (std::is_same_v<Message, wire::FrameKey>) {
                // From the sender itself: never passed on.
                if (find_peer(peer)) sealing_->keep(peer, m.kid, m.key);
            } else if constexpr (std::is_same_v<Message, wire::Chat>) {
                on_chat(peer, ChatMessage{.sent = m.sent, .text = std::move(m.text)},
                        /*told=*/false);
            } else if constexpr (std::is_same_v<Message, wire::ChatAsk>) {
                tell_chat(peer);
            } else if constexpr (std::is_same_v<Message, wire::ChatTold>) {
                on_chat(peer, std::move(m.message), /*told=*/true);
            } else {
                static_assert(std::is_same_v<Message, wire::KeyframeRequest>);
                on_keyframe_request(peer, m.stream);
            }
        },
        std::move(*control));
}

void MediaSession::on_frame(const PeerId& origin, const PeerId& from,
                            std::span<const uint8_t> message, std::optional<Timestamp> deadline) {
    const Result<wire::FrameHeader> header = wire::read_frame_header(message);
    if (!header) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::shared_ptr<IncomingStream> stream = find_incoming(origin, header->info.stream);
    if (!stream) return;  // withdrawn while the frame was in flight

    // Without a deadline from the network (TCP), a frame passed on gets its
    // lifetime again, from when it is reckoned to have left the node it came
    // from.
    const bool relays = stream->relay_slot.load(std::memory_order_relaxed) != 0;
    const std::shared_ptr<PeerState> sender = relays && !deadline ? find_peer(from) : nullptr;
    const Timestamp now = clock_.now();
    std::lock_guard delivery(stream->delivery);
    {
        std::lock_guard lock(stream->mutex);
        if (!stream->subscribed) return;
        stream->bytes += message.size();
        if (!stream->relayed.copies.empty()) {
            Duration left = deadline ? *deadline - now : stream->description.lifetime;
            if (sender) left -= sender->rtt() / 2;
            pass_on(origin, *stream, header->info, message, left, now);
        }
        if (config_.forwarder) return;  // passed on, and that is all
        std::optional<Buffer> payload = open(origin, from, *header, message);
        if (!payload) {
            stream->unopened += 1;
            return;
        }
        if (header->sealed) stream->opened += 1;
        stream->tracker.push(EncodedFrame{header->info, std::move(*payload)}, now);
    }
    deliver_ready(origin, header->info.stream, *stream, now);
}

void MediaSession::pass_on(const PeerId& origin, IncomingStream& stream, const FrameInfo& info,
                           std::span<const uint8_t> message, Duration left, Timestamp now) {
    Fanout& relayed = stream.relayed;
    const MediaKind kind = stream.description.kind();
    bool want_keyframe = false;
    if (left <= Duration::zero()) {
        want_keyframe = relayed.miss(info, kind != MediaKind::Video);
    } else {
        const uint64_t tag =
            kind == MediaKind::Video
                ? live_tag(stream.relay_slot.load(std::memory_order_relaxed), info.number)
                : 0;
        want_keyframe = *relayed.send(
            network_, info, kind, left, tag, now,
            [&]() -> Result<Buffer> { return wire::encode_relayed_frame(origin, message); });
    }
    if (want_keyframe) ask_keyframe_for_relayed(origin, info.stream, stream, now);
}

void MediaSession::ask_keyframe_for_relayed(const PeerId& origin, StreamId id,
                                            IncomingStream& stream, Timestamp now) {
    if (stream.keyframe_asked && now - *stream.keyframe_asked < config_.min_keyframe_interval) {
        return;  // the next frame that needs one asks again
    }
    stream.keyframe_asked = now;
    network_.send_reliable(origin, wire::encode_control(wire::KeyframeRequest{id}));
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
    give_key(peer);  // before its first frame

    bool ask = false;
    {
        std::lock_guard lock(stream->mutex);
        if (stream->subscriber_of(peer)) return;
        stream->fanout.copies.emplace_back(std::move(state));
    }
    // One more copy through an uplink known to be full: through a relay where
    // it is short, and down to fit before its keyframe is made, which would
    // otherwise be the one to overfill it.
    arrange(*stream);
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
            if (all.per_most(uplink) && all.paths.contains(it->first)) {
                ++it;
                continue;
            }
            told.emplace_back(it->first, 0);
            it = ceilings_.erase(it);
        }
        if (all.per_most(uplink)) {
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
            if (!s.rate || s.fanout.copies.empty()) continue;
            const uint32_t before = s.rate->target();
            bitrate = s.count(s.rate->limit(static_cast<uint64_t>(per_most * s.rate->limits().max)),
                              before);
        }
        if (bitrate) notify(handlers_.bitrate_changed, id, *bitrate);
    }
}

void MediaSession::on_unsubscribe(const PeerId& peer, StreamId stream_id) {
    if (const std::shared_ptr<OutgoingStream> stream = find_outgoing(stream_id))
        drop_subscriber(stream_id, *stream, peer);
}

void MediaSession::drop_subscriber(StreamId id, OutgoingStream& stream, const PeerId& peer) {
    std::optional<std::pair<PeerId, Buffer>> told;
    bool ask = false;
    std::optional<uint32_t> bitrate;
    {
        std::lock_guard lock(stream.mutex);
        OutgoingStream& s = stream;
        const Copy* copy = s.subscriber_of(peer);
        if (!copy) return;
        const std::shared_ptr<PeerState> gone = copy->peer;
        const std::shared_ptr<PeerState> relay = copy->relay;
        std::erase_if(s.fanout.copies, [&](const Copy& c) { return c.peer == gone; });
        // Those it passed the stream on to get it from here again.
        bool orphaned = false;
        for (Copy& c : s.fanout.copies) {
            if (c.relay != gone) continue;
            c.relay.reset();
            c.moved();
            orphaned = true;
        }
        ask = orphaned && !s.stands_alone() && ask_keyframe(s, clock_.now());
        if (relay) told.emplace(relay->id, s.forward_message(*relay));
        s.drop_idle_forwarders();
        bitrate = s.restart_if_unwatched();
    }
    if (told) network_.send_reliable(told->first, told->second);
    // Those left to this node go to another relay where its uplink is short.
    arrange(stream);
    if (ask) notify(handlers_.keyframe_needed, id);
    if (bitrate) notify(handlers_.bitrate_changed, id, *bitrate);
}

void MediaSession::on_keyframe_request(const PeerId& peer, StreamId stream_id) {
    const std::shared_ptr<OutgoingStream> stream = find_outgoing(stream_id);
    if (!stream) return;

    const Timestamp now = clock_.now();
    bool ask = false;
    {
        std::lock_guard lock(stream->mutex);
        OutgoingStream& s = *stream;
        // From a subscriber, whichever way its frames go, or from a relay for
        // one it passes them on to.
        Copy* subscriber = s.subscriber_of(peer);
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

// ── Relaying ────────────────────────────────────────────────────────────────

Result<void> MediaSession::relay_through(StreamId stream_id, const PeerId& viewer,
                                         std::optional<PeerId> relay) {
    const std::shared_ptr<OutgoingStream> stream = find_outgoing(stream_id);
    if (!stream) return fail(Errc::InvalidArgument);
    std::vector<std::pair<PeerId, Buffer>> told;
    bool ask = false;
    {
        std::lock_guard lock(stream->mutex);
        OutgoingStream& s = *stream;
        Copy* copy = s.subscriber_of(viewer);
        if (!copy) return fail(Errc::InvalidArgument);
        std::shared_ptr<PeerState> through;
        if (relay) {
            const Copy* by = s.subscriber_of(*relay);
            if (!by || *relay == viewer) return fail(Errc::InvalidArgument);
            // No loop: the relay's own frames may not come through the viewer.
            for (const Copy* up = by; up && up->relay; up = s.subscriber_of(up->relay->id)) {
                if (up->relay->id == viewer) return fail(Errc::InvalidArgument);
            }
            through = by->peer;
        }
        ask = s.move(*copy, std::move(through), told) && !s.stands_alone() &&
              ask_keyframe(s, clock_.now());
    }
    for (const auto& [peer, message] : told) network_.send_reliable(peer, message);
    if (ask) notify(handlers_.keyframe_needed, stream_id);
    return {};
}

void MediaSession::arrange(OutgoingStream& stream) {
    if (!config_.relays) return;
    const Forwarders forwarders = this->forwarders();
    std::vector<std::pair<PeerId, Buffer>> told;
    if (!forwarders.empty()) {
        // Carried by a server anyway: through its forwarder, one copy for all
        // of them where each would take one through the same server, and no
        // later for it — so whatever the round trips, which are not yet known
        // as viewers first subscribe. Asked of the network outside the
        // stream's lock.
        std::vector<PeerId> carried;
        {
            std::lock_guard lock(stream.mutex);
            if (!stream.relayable()) return;
            for (const Copy& copy : stream.fanout.copies) {
                if (copy.direct() && !copy.peer->is_forwarder()) carried.push_back(copy.peer->id);
            }
        }
        std::erase_if(carried, [&](const PeerId& peer) { return !network_.relayed(peer); });
        std::lock_guard lock(stream.mutex);
        OutgoingStream& s = stream;
        for (const PeerId& peer : carried) {
            const Copy* viewer = s.direct_subscriber_of(peer);
            if (!viewer || s.passed_on_by(*viewer->peer) != 0) continue;
            if (std::shared_ptr<PeerState> forwarder =
                    s.relay_for(*viewer, forwarders, /*new_forwarder=*/true, /*carried=*/true)) {
                s.move(*s.subscriber_of(peer), std::move(forwarder), told);
            }
        }
    }
    if (const std::optional<uint64_t> uplink = uplink_capacity()) {
        double weight = copies().weight;
        std::lock_guard lock(stream.mutex);
        OutgoingStream& s = stream;
        if (s.relayable()) {
            const double most = s.rate->limits().max;
            while (weight > 0 && double(*uplink) * SharedUplink::kCopies / weight < kShort) {
                const std::optional<int> taken = s.hand_off(forwarders, told);
                if (!taken) break;
                weight -= most * *taken;
            }
        }
    }
    for (const auto& [peer, message] : told) network_.send_reliable(peer, message);
}

void MediaSession::arrange_all() {
    for (const auto& [id, stream] : outgoing_streams()) arrange(*stream);
}

void MediaSession::tell_uplink(std::optional<uint64_t> capacity) {
    const Buffer message = wire::encode_control(wire::Uplink{capacity});
    for (const PeerId& peer : connected_peers()) network_.send_reliable(peer, message);
}

void MediaSession::relieve(const PeerId& relay) {
    if (!config_.relays) return;
    const std::shared_ptr<PeerState> state = find_peer(relay);
    if (!state) return;
    const Forwarders forwarders = this->forwarders();
    for (const auto& [id, stream] : outgoing_streams()) {
        const std::optional<uint64_t> uplink = uplink_capacity();
        const double weight = copies().weight;
        std::vector<std::pair<PeerId, Buffer>> told;
        bool ask = false;
        {
            std::lock_guard lock(stream->mutex);
            OutgoingStream& s = *stream;
            if (!s.relayable()) continue;
            Copy* viewer = s.last_leaf(state.get());
            if (!viewer) continue;
            // Back to this node, where its uplink has room for one more copy;
            // to another relay otherwise, if there is one.
            const double most = s.rate->limits().max;
            const bool room =
                !uplink || double(*uplink) * SharedUplink::kCopies / (weight + most) >= kShort;
            std::shared_ptr<PeerState> to;
            const PeerId moved = viewer->peer->id;
            if (!room) {
                to = s.relay_for(*viewer, forwarders, /*new_forwarder=*/true);
                if (!to) continue;
            }
            ask = s.move(*s.subscriber_of(moved), std::move(to), told) &&
                  ask_keyframe(s, clock_.now());
        }
        for (const auto& [peer, message] : told) network_.send_reliable(peer, message);
        if (ask) notify(handlers_.keyframe_needed, id);
    }
}

void MediaSession::on_uplink(const PeerId& peer, std::optional<uint64_t> capacity) {
    const std::shared_ptr<PeerState> state = find_peer(peer);
    if (!state) return;
    state->uplink_full.store(capacity.has_value(), std::memory_order_relaxed);
    if (capacity) {
        relieve(peer);
    } else {
        arrange_all();  // one more relay with room
    }
}

void MediaSession::on_forward(const PeerId& sender, const wire::Forward& forward) {
    const std::shared_ptr<IncomingStream> stream = find_incoming(sender, forward.stream);
    if (!stream) return;  // withdrawn while it was on its way
    // Only to the viewers this node is connected to; of the others the sender
    // hears.
    std::vector<std::shared_ptr<PeerState>> viewers;
    for (const PeerId& viewer : forward.viewers) {
        if (viewer == sender) continue;
        if (std::shared_ptr<PeerState> state = find_peer(viewer)) {
            viewers.push_back(std::move(state));
        } else {
            network_.send_reliable(sender,
                                   wire::encode_control(wire::CannotReach{forward.stream, viewer}));
        }
    }
    if (!viewers.empty() && stream->relay_slot.load(std::memory_order_relaxed) == 0) {
        std::unique_lock lock(mutex_);
        if (stream->relay_slot.load(std::memory_order_relaxed) == 0)
            stream->relay_slot.store(take_slot(), std::memory_order_relaxed);
    }

    std::unique_lock lock(stream->mutex);
    Fanout& relayed = stream->relayed;
    if (config_.forwarder && viewers.empty() != !stream->subscribed) {
        // A forwarder takes a stream as it is handed viewers of it — its
        // sender sends it the frames already — and lets go as it is left
        // none, which its sender knows.
        stream->subscribed = !viewers.empty();
        stream->tracker.reset(clock_.now());
        if (!stream->subscribed) {
            relayed.copies.clear();
            lock.unlock();
            network_.send_reliable(sender, wire::encode_control(wire::Unsubscribe{forward.stream}));
            return;
        }
    }
    if (!stream->subscribed) {
        // Its sender hands them on as it hears this node no longer watches.
        relayed.copies.clear();
        return;
    }
    stream->relayed_most = forward.most;
    // Those it already passes the stream on to go on as they were.
    std::vector<Copy> copies;
    for (std::shared_ptr<PeerState>& viewer : viewers) {
        const auto listed = [&](const Copy& copy) { return copy.peer == viewer; };
        if (std::ranges::any_of(copies, listed)) continue;
        const Copy* had = relayed.find(viewer->id);
        copies.push_back(had ? *had : Copy(std::move(viewer)));
    }
    relayed.copies = std::move(copies);
}

// ── Sealing ─────────────────────────────────────────────────────────────────

void MediaSession::give_key(const PeerId& peer, bool again) {
    if (!config_.seal) return;
    if (!again && !entitled(peer)) return;
    std::vector<Buffer> messages;
    {
        std::lock_guard lock(sealing_->mutex);
        const bool keyed =
            again ? sealing_->keyed.contains(peer) : sealing_->keyed.insert(peer).second;
        if (!keyed) return;
        messages = sealing_->messages();
    }
    for (const Buffer& message : messages) network_.send_reliable(peer, message);
}

void MediaSession::rekey_without(const PeerId& peer, Timestamp now) {
    std::vector<PeerId> keyed;
    Buffer message;
    {
        std::lock_guard lock(sealing_->mutex);
        sealing_->given.erase(peer);
        if (sealing_->keyed.erase(peer) == 0) return;
        // A next key it was told goes too: another in its place.
        sealing_->next = sealing_->make(now + kRekeyAfter);
        message =
            wire::encode_control(wire::FrameKey{sealing_->next->key.kid(), sealing_->next->base});
        keyed.assign(sealing_->keyed.begin(), sealing_->keyed.end());
    }
    for (const PeerId& other : keyed) network_.send_reliable(other, message);
}

std::optional<Buffer> MediaSession::open(const PeerId& origin, const PeerId& from,
                                         const wire::FrameHeader& header,
                                         std::span<const uint8_t> message) {
    if (!header.sealed) {
        // Its sender's own word, over a connection only it holds; passed on by
        // another, anyone's.
        if (config_.seal && from != origin) return std::nullopt;
        return Buffer::copy_of(message).slice(header.size);
    }
    const std::span<const uint8_t> sealed = message.subspan(header.size);
    const Result<sframe::ReadHeader> read = sframe::read_header(sealed);
    if (!read) return std::nullopt;
    const std::optional<sframe::Key> key = sealing_->of(origin, read->header.kid);
    if (!key) return std::nullopt;
    Result<Buffer> plain = key->open(*read, message.first(header.size), sealed);
    if (!plain) return std::nullopt;
    return std::move(*plain);
}

void MediaSession::on_forwarder(const PeerId& peer) {
    const std::shared_ptr<PeerState> state = find_peer(peer);
    if (!state || state->forwarder.exchange(true)) return;
    arrange_all();  // one more relay, always with room
}

void MediaSession::on_cannot_reach(const PeerId& relay, const wire::CannotReach& cannot) {
    const std::shared_ptr<OutgoingStream> stream = find_outgoing(cannot.stream);
    if (!stream) return;
    std::vector<std::pair<PeerId, Buffer>> told;
    bool ask = false;
    {
        std::lock_guard lock(stream->mutex);
        OutgoingStream& s = *stream;
        Copy* copy = s.subscriber_of(cannot.viewer);
        if (!copy || !copy->relay || copy->relay->id != relay) return;
        copy->not_through.push_back(relay);
        ask = s.move(*copy, nullptr, told) && ask_keyframe(s, clock_.now());
    }
    for (const auto& [peer, message] : told) network_.send_reliable(peer, message);
    arrange(*stream);  // through another, where the uplink is short
    if (ask) notify(handlers_.keyframe_needed, cannot.stream);
}

void MediaSession::on_relayed_path(const PeerId& relay, const wire::RelayedPath& report) {
    const std::shared_ptr<OutgoingStream> stream = find_outgoing(report.stream);
    if (!stream) return;
    std::optional<uint32_t> bitrate;
    bool too_far = false;
    bool forwarder = false;
    {
        std::lock_guard lock(stream->mutex);
        OutgoingStream& s = *stream;
        Copy* copy = s.rate ? s.subscriber_of(report.viewer) : nullptr;
        const Copy* through = s.subscriber_of(relay);
        // From the relay its frames go through, while they do.
        if (!copy || !through || !copy->relay || copy->relay->id != relay) return;
        copy->backpressured = copy->backpressured || report.backpressured;
        copy->relayed_rtt = report.signal.rtt - report.signal.queue_rise;
        too_far = !in_time(s.reach(*through), *copy->relayed_rtt, s.description.lifetime);
        forwarder = through->peer->is_forwarder();
        bitrate = s.follow(*copy, report.signal, std::nullopt, clock_.now());
    }
    if (bitrate) notify(handlers_.bitrate_changed, report.stream, *bitrate);
    // Its frames cannot arrive in time that way: as if the relay could not
    // reach it at all — unless it is a forwarder whose server carries the
    // viewer anyway, its frames that way however sent.
    if (too_far && !(forwarder && network_.relayed(report.viewer)))
        on_cannot_reach(relay, wire::CannotReach{report.stream, report.viewer});
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
            if (!stream->subscribed || config_.forwarder) continue;
            stream->tracker.update(now);
        }
        deliver_ready(key.peer, key.stream, *stream, now);
    }
}

uint64_t MediaSession::rejected_messages() const noexcept {
    return rejected_.load(std::memory_order_relaxed);
}

}  // namespace ratcast
