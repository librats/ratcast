// MediaSession end to end over an in-memory network, in virtual time: every
// message is queued, and delivered only when the test says so — which is
// where it drops, reorders or holds them. The sender hears what became of a
// live message as it is delivered or dropped.

#include "ratcast/session/media_session.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "wire/control_codec.h"
#include "wire/frame_codec.h"
#include "wire/message.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

PeerId make_peer(uint8_t seed) {
    std::array<uint8_t, 32> key{};
    key.fill(seed);
    return PeerId::from_public_key(key.data(), key.size());
}

struct Message {
    PeerId from;
    PeerId to;
    Buffer bytes;
    bool live = false;
    uint64_t tag = 0;  ///< a live message's, for its outcome
    /// A live message's: when it is due, as librats tells a receiver.
    std::optional<Timestamp> deadline = std::nullopt;
};

/// The number of the frame a live message carries.
FrameNumber number_of(const Message& m) {
    return wire::read_frame_header(m.bytes.bytes())->info.number;
}

/// The wire between every session in a test.
class Hub {
public:
    std::deque<Message> queue;
    std::map<PeerId, MediaSession*> sessions;
    /// Live messages it returns true for are lost.
    std::function<bool(const Message&)> lose;
    /// Live messages it returns true for stay on their way, in `held`, until
    /// release().
    std::function<bool(const Message&)> hold;
    std::deque<Message> held;
    /// Control messages delivered, by kind.
    std::map<wire::MessageKind, int> delivered;

    /// Deliver everything queued, including what deliveries queue in turn.
    void deliver_all() {
        while (!queue.empty()) deliver_front();
    }

    /// Deliver what is queued now; what deliveries queue in turn waits.
    void deliver_queued() {
        for (size_t n = queue.size(); n > 0; --n) deliver_front();
    }

    void deliver_front() {
        Message m = std::move(queue.front());
        queue.pop_front();
        if (m.live && hold && hold(m)) {
            held.push_back(std::move(m));
            return;
        }
        const bool lost = m.live && lose && lose(m);
        if (!m.live) ++delivered[*wire::peek_kind(m.bytes.bytes())];
        if (!lost) sessions.at(m.to)->on_message(m.from, m.bytes.bytes(), m.deadline);
        if (m.live && m.tag != 0) sessions.at(m.from)->on_live_outcome(m.to, m.tag, !lost);
    }

    /// Deliver what was held, and stop holding.
    void release() {
        hold = nullptr;
        while (!held.empty()) {
            queue.push_front(std::move(held.back()));
            held.pop_back();
        }
        deliver_all();
    }

    /// Live messages queued right now, in reverse order: what a path that
    /// repairs the earlier ones late looks like.
    void reverse_live() {
        std::vector<Message> live;
        std::deque<Message> rest;
        for (Message& m : queue) {
            if (m.live) {
                live.push_back(std::move(m));
            } else {
                rest.push_back(std::move(m));
            }
        }
        std::ranges::reverse(live);
        for (Message& m : live) rest.push_back(std::move(m));
        queue = std::move(rest);
    }
};

class Endpoint final : public MediaNetwork {
public:
    Endpoint(Hub& hub, Clock& clock, PeerId self)
        : hub_(hub), clock_(clock), self_(std::move(self)) {}

    void send_reliable(const PeerId& peer, const Buffer& message) override {
        hub_.queue.push_back({self_, peer, message, false});
    }
    bool send_live(const PeerId& peer, const Buffer& message, Duration lifetime, uint64_t tag,
                   LiveRank rank) override {
        EXPECT_GT(lifetime, Duration::zero());
        lifetimes.push_back(lifetime);
        tags.push_back(tag);
        ranks.push_back(rank);
        hub_.queue.push_back({self_, peer, message, true, tag, clock_.now() + lifetime});
        return !full_.contains(peer);
    }
    bool writable(const PeerId& peer) const override { return !full_.contains(peer); }
    size_t max_live_message() const override { return 64 * 1024; }
    bool relayed(const PeerId& peer) const override { return carried.contains(peer); }
    void set_send_ceiling(const PeerId& peer, uint64_t bits_per_second) override {
        if (bits_per_second == 0) {
            ceilings.erase(peer);
        } else {
            ceilings[peer] = bits_per_second;
        }
    }

    /// The send ceilings set, by peer.
    std::map<PeerId, uint64_t> ceilings;
    /// The peers a server carries the connection to (MediaNetwork::relayed()).
    std::set<PeerId> carried;
    /// What each live message was given, in the order they were sent.
    std::vector<Duration> lifetimes;
    std::vector<uint64_t> tags;
    std::vector<LiveRank> ranks;

    /// Make `peer`'s queue look full, or drained.
    void set_full(const PeerId& peer, bool full) {
        if (full) {
            full_.insert(peer);
        } else {
            full_.erase(peer);
        }
    }

private:
    Hub& hub_;
    Clock& clock_;
    PeerId self_;
    std::set<PeerId> full_;
};

/// One participant: a session, its network endpoint and what its handlers saw.
struct Party {
    Party(Hub& hub, Clock& clock, uint8_t seed, SessionConfig config = {})
        : id(make_peer(seed)),
          endpoint(hub, clock, id),
          session(endpoint, clock, handlers(), config) {
        hub.sessions[id] = &session;
    }

    SessionHandlers handlers() {
        return SessionHandlers{
            .stream_announced = [this](const PeerId&,
                                       const StreamDescription& d) { announced.push_back(d); },
            .stream_withdrawn = [this](const PeerId&, StreamId s) { withdrawn.push_back(s); },
            .frame_received =
                [this](const PeerId&, EncodedFrame f) {
                    if (on_frame) on_frame(f);
                    frames.push_back(f.info.number);
                    payloads.push_back(f.payload);
                },
            .keyframe_needed = [this](StreamId) { ++keyframes_needed; },
            .bitrate_changed = [this](StreamId, uint32_t b) { bitrates.push_back(b); },
            .presence_changed = [this](const PeerId&,
                                       const Presence& p) { presences.push_back(p); },
        };
    }

    PeerId id;
    Endpoint endpoint;
    std::vector<StreamDescription> announced;
    std::vector<StreamId> withdrawn;
    std::vector<FrameNumber> frames;
    std::vector<Buffer> payloads;
    int keyframes_needed = 0;
    std::vector<uint32_t> bitrates;
    std::vector<Presence> presences;  ///< what peers said of themselves, in order
    std::function<void(const EncodedFrame&)> on_frame;
    MediaSession session;  // last: its handlers point at the members above
};

StreamDescription screen(StreamId id = 1) {
    StreamDescription d;
    d.id = id;
    d.codec = Codec::H264;
    d.format = VideoFormat{1920, 1080, 60, 1};
    d.lifetime = 150ms;
    d.label = "screen";
    return d;
}

StreamDescription mic(StreamId id = 3) {
    StreamDescription d;
    d.id = id;
    d.codec = Codec::Opus;
    d.format = AudioFormat{48'000, 1};
    d.lifetime = 100ms;
    d.label = "mic";
    return d;
}

StreamDescription feed(StreamId id = 4) {
    StreamDescription d;
    d.id = id;
    d.codec = Codec::MpegTs;
    d.format = DataFormat{};
    d.lifetime = 500ms;
    d.label = "feed";
    return d;
}

MutableBuffer payload_for(FrameNumber number, size_t size = 300) {
    MutableBuffer payload = allocate_frame_payload(size);
    uint8_t* bytes = payload.append(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(number + i);
    return payload;
}

constexpr BitrateLimits kBitrate{.min = 250'000, .start = 2'000'000, .max = 8'000'000};

class MediaSessionTest : public ::testing::Test {
protected:
    MediaSessionTest() {
        connect(sender, receiver);
        EXPECT_TRUE(sender.session.add_stream(screen()));
        hub.deliver_all();
    }

    void connect(Party& a, Party& b) {
        a.session.on_peer_connected(b.id);
        b.session.on_peer_connected(a.id);
    }

    void subscribe(Party& who, StreamId stream = 1) {
        ASSERT_TRUE(who.session.subscribe(sender.id, stream));
        hub.deliver_all();
    }

    /// Stream 2, rate-controlled.
    void add_rated_stream() {
        ASSERT_TRUE(sender.session.add_stream(screen(2), kBitrate));
        hub.deliver_all();
    }

    Result<void> send(FrameNumber number, bool keyframe, StreamId stream = 1) {
        FrameInfo info;
        info.number = number;
        info.keyframe = keyframe;
        info.capture_time = clock.now();
        if (!keyframe && number > 0) info.references = {number - 1};
        return sender.session.send_frame(stream, info, payload_for(number));
    }

    /// Let `d` pass. What is already in flight arrives first: the network is
    /// fast, it is the time in between that the test is about.
    void advance(Duration d) {
        hub.deliver_all();
        clock.advance(d);
        sender.session.tick();
        receiver.session.tick();
        hub.deliver_all();
    }

    ManualClock clock{Timestamp{100s}};
    Hub hub;
    /// Every copy from the sender: its viewers here are not connected to each
    /// other, and the tree is TreeTest's.
    Party sender{hub, clock, 1, SessionConfig{.relays = false}};
    Party receiver{hub, clock, 2};
};

using Frames = std::vector<FrameNumber>;
using Bitrates = std::vector<uint32_t>;

/// A path the sender does not fill, `expired` live messages since it opened.
PathStats path(uint64_t expired = 0) {
    PathStats s;
    s.valid = true;
    s.bandwidth_bps = 20'000'000;
    s.app_limited = true;
    s.srtt_us = 50'000;
    s.live_expired = expired;
    return s;
}

/// The same while the transport is still finding it, as BBR's Startup does.
PathStats starting_path(uint64_t expired) {
    PathStats s = path(expired);
    s.pacing_bps = s.bandwidth_bps * 277 / 100;
    return s;
}

}  // namespace

// ── Announcing ──────────────────────────────────────────────────────────────

TEST_F(MediaSessionTest, AnnouncesStreamsToConnectedPeers) {
    ASSERT_EQ(receiver.announced.size(), 1u);
    EXPECT_EQ(receiver.announced[0], screen());
}

TEST_F(MediaSessionTest, AnnouncesExistingStreamsToPeersThatConnectLater) {
    Party late(hub, clock, 3);
    connect(sender, late);
    hub.deliver_all();
    ASSERT_EQ(late.announced.size(), 1u);
    EXPECT_EQ(late.announced[0].label, "screen");
}

TEST_F(MediaSessionTest, AnnouncesChangesAndWithdrawals) {
    StreamDescription changed = screen();
    changed.format = VideoFormat{1280, 720, 30, 1};
    ASSERT_TRUE(sender.session.update_stream(changed));
    hub.deliver_all();
    ASSERT_EQ(receiver.announced.size(), 2u);
    EXPECT_EQ(receiver.announced[1], changed);

    sender.session.remove_stream(1);
    hub.deliver_all();
    EXPECT_EQ(receiver.withdrawn, std::vector<StreamId>{1});
    EXPECT_FALSE(receiver.session.subscribe(sender.id, 1));
}

TEST_F(MediaSessionTest, WhatPeersOfferIsWhatTheyAnnouncedAndHaveNotWithdrawn) {
    ASSERT_TRUE(sender.session.add_stream(screen(2)));
    StreamDescription changed = screen();
    changed.format = VideoFormat{1280, 720, 30, 1};
    ASSERT_TRUE(sender.session.update_stream(changed));
    Party other(hub, clock, 3);
    connect(other, receiver);
    ASSERT_TRUE(other.session.add_stream(mic()));
    hub.deliver_all();
    // By peer, then by id.
    std::vector<OfferedStream> offered{
        {sender.id, changed}, {sender.id, screen(2)}, {other.id, mic()}};
    if (other.id < sender.id) std::ranges::rotate(offered, offered.begin() + 2);
    EXPECT_EQ(receiver.session.offered(), offered);
    EXPECT_TRUE(sender.session.offered().empty()) << "its own are not offered to it";

    sender.session.remove_stream(1);
    hub.deliver_all();
    receiver.session.on_peer_disconnected(other.id);
    EXPECT_EQ(receiver.session.offered(), (std::vector<OfferedStream>{{sender.id, screen(2)}}));
}

TEST_F(MediaSessionTest, RefusesStreamsItCannotOffer) {
    StreamDescription invalid = screen(2);
    invalid.lifetime = 0ms;
    EXPECT_EQ(sender.session.add_stream(invalid).error(), Errc::InvalidArgument);
    EXPECT_EQ(sender.session.add_stream(screen(1)).error(), Errc::InvalidArgument);  // id in use
    EXPECT_EQ(sender.session.update_stream(screen(9)).error(), Errc::InvalidArgument);
}

// ── Subscribing and sending ─────────────────────────────────────────────────

TEST_F(MediaSessionTest, SubscriberStartsAtTheNextKeyframe) {
    EXPECT_EQ(sender.keyframes_needed, 0);
    subscribe(receiver);
    EXPECT_EQ(sender.keyframes_needed, 1);  // the new subscriber needs one

    ASSERT_TRUE(send(0, false));  // the encoder has not caught up yet
    ASSERT_TRUE(send(1, true));
    ASSERT_TRUE(send(2, false));
    hub.deliver_all();

    EXPECT_EQ(receiver.frames, (Frames{1, 2}));
    const auto stats = sender.session.outgoing_stats(1);
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->subscribers, 1u);
    EXPECT_EQ(stats->awaiting_keyframe, 1u);
    EXPECT_EQ(stats->messages, 2u);
}

TEST_F(MediaSessionTest, DeliversPayloadsIntact) {
    subscribe(receiver);
    ASSERT_TRUE(send(0, true));
    hub.deliver_all();
    ASSERT_EQ(receiver.payloads.size(), 1u);
    const MutableBuffer expected = payload_for(0);
    EXPECT_TRUE(std::ranges::equal(receiver.payloads[0].bytes(), expected.bytes()));
}

TEST_F(MediaSessionTest, SendsNothingWithoutSubscribers) {
    ASSERT_TRUE(send(0, true));
    EXPECT_TRUE(hub.queue.empty());

    subscribe(receiver);
    receiver.session.unsubscribe(sender.id, 1);
    hub.deliver_all();
    ASSERT_TRUE(send(1, true));
    EXPECT_TRUE(hub.queue.empty());
    EXPECT_TRUE(receiver.frames.empty());
}

TEST_F(MediaSessionTest, RefusesFramesItCannotSend) {
    ASSERT_TRUE(send(5, true));
    EXPECT_EQ(send(5, true).error(), Errc::InvalidArgument);  // numbers must increase
    EXPECT_EQ(send(4, true).error(), Errc::InvalidArgument);

    FrameInfo info;
    info.number = 6;
    EXPECT_EQ(sender.session.send_frame(99, info, payload_for(6)).error(), Errc::InvalidArgument);
    EXPECT_EQ(sender.session.send_frame(1, info, payload_for(6, 100 * 1024)).error(),
              Errc::InvalidArgument);  // past max_live_message
}

TEST_F(MediaSessionTest, HandsFramesOnInDecodeOrder) {
    subscribe(receiver);
    for (FrameNumber n = 0; n < 5; ++n) ASSERT_TRUE(send(n, n == 0));
    hub.reverse_live();
    hub.deliver_all();
    EXPECT_EQ(receiver.frames, (Frames{0, 1, 2, 3, 4}));
}

// ── Recovering ──────────────────────────────────────────────────────────────

TEST_F(MediaSessionTest, ALostReferenceBringsAKeyframe) {
    subscribe(receiver);
    ASSERT_TRUE(send(0, true));
    hub.deliver_all();
    advance(1s);  // well past the keyframe interval

    hub.lose = [](const Message&) { return true; };
    ASSERT_TRUE(send(1, false));
    EXPECT_EQ(sender.endpoint.ranks.back(), LiveRank::Frame);
    hub.deliver_all();
    hub.lose = nullptr;
    // The network gave frame 1 up: the receiver cannot decode what follows.
    EXPECT_EQ(sender.keyframes_needed, 2);
    ASSERT_TRUE(send(2, false));
    hub.deliver_all();
    EXPECT_EQ(receiver.frames, Frames{0});

    advance(150ms);  // the receiver gives frame 1 up as well, and asks
    EXPECT_EQ(sender.session.outgoing_stats(1)->keyframe_requests, 1u);
    ASSERT_TRUE(send(3, true));
    advance(1s);
    EXPECT_EQ(sender.keyframes_needed, 2);  // the one keyframe answers both
    EXPECT_EQ(receiver.frames, (Frames{0, 3}));

    const auto stats = receiver.session.incoming_stats(sender.id, 1);
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->frames.lost, 1u);
    EXPECT_EQ(stats->frames.undecodable, 1u);
}

TEST_F(MediaSessionTest, AFrameGivenUpOnTheWayIsAKeyframeNeededAtOnce) {
    subscribe(receiver);
    ASSERT_TRUE(send(0, true));
    hub.deliver_all();
    EXPECT_EQ(sender.keyframes_needed, 1);
    ASSERT_EQ(sender.endpoint.tags.size(), 1u);
    EXPECT_NE(sender.endpoint.tags[0], 0u);  // the network is to say what became of it
    EXPECT_EQ(sender.endpoint.ranks[0], LiveRank::Keyframe);

    // Given up on right behind the keyframe: the receiver cannot decode what
    // follows, and the sender need not wait for it to say so.
    hub.lose = [](const Message&) { return true; };
    ASSERT_TRUE(send(1, false));
    hub.deliver_all();
    EXPECT_EQ(sender.keyframes_needed, 1);  // held back: a keyframe was just sent...
    advance(200ms);
    EXPECT_EQ(sender.keyframes_needed, 2);  // ...until min_keyframe_interval has passed

    // A frame given up on after a keyframe that went behind it is no loss.
    hub.lose = nullptr;
    ASSERT_TRUE(send(2, true));
    advance(1s);
    hub.lose = [](const Message& m) { return number_of(m) == 3; };
    ASSERT_TRUE(send(3, false));
    ASSERT_TRUE(send(4, true));
    hub.deliver_all();
    EXPECT_EQ(sender.keyframes_needed, 2);

    // Nor is what the path's samples say: they count what expired, not which.
    sender.session.on_peer_path(receiver.id, path(3));
    advance(1s);
    EXPECT_EQ(sender.keyframes_needed, 2);
}

// ── Audio ───────────────────────────────────────────────────────────────────

// Every audio frame decodes on its own: a subscriber starts at once, a lost
// frame costs only itself, and nobody asks for a keyframe.
TEST_F(MediaSessionTest, AnAudioStreamNeedsNoKeyframes) {
    ASSERT_TRUE(sender.session.add_stream(mic()));
    hub.deliver_all();
    subscribe(receiver, 3);
    EXPECT_EQ(sender.keyframes_needed, 0);

    hub.lose = [](const Message& m) { return number_of(m) == 4; };
    for (FrameNumber n = 0; n < 10; ++n) ASSERT_TRUE(send(n, true, 3));
    advance(1s);
    EXPECT_EQ(receiver.frames, (Frames{0, 1, 2, 3, 5, 6, 7, 8, 9}));
    EXPECT_EQ(sender.keyframes_needed, 0);
    EXPECT_EQ(sender.session.outgoing_stats(3)->keyframes_asked, 0u);

    // Every frame with the stream's own lifetime, none of them tagged: what
    // became of one changes nothing.
    for (Duration lifetime : sender.endpoint.lifetimes) EXPECT_EQ(lifetime, 100ms);
    for (uint64_t tag : sender.endpoint.tags) EXPECT_EQ(tag, 0u);
    for (LiveRank rank : sender.endpoint.ranks) EXPECT_EQ(rank, LiveRank::Sound);

    // Even a receiver that asks gets none.
    receiver.session.request_keyframe(sender.id, 3);
    advance(1s);
    EXPECT_EQ(sender.keyframes_needed, 0);
}

// Audio frames go on as they arrive, for a jitter buffer to put in order:
// waiting for a gap would take the time it has to wait for late frames.
TEST_F(MediaSessionTest, AudioFramesAreHandedOnAsTheyArrive) {
    ASSERT_TRUE(sender.session.add_stream(mic()));
    hub.deliver_all();
    subscribe(receiver, 3);
    for (FrameNumber n = 0; n < 4; ++n) ASSERT_TRUE(send(n, true, 3));
    hub.reverse_live();
    hub.deliver_all();
    EXPECT_EQ(receiver.frames, (Frames{3, 2, 1, 0}));
}

// A data stream is carried as sound is, ranked with the pictures: nothing in
// it is worth more than the rest.
TEST_F(MediaSessionTest, ADataStreamIsCarriedAsSoundIs) {
    ASSERT_TRUE(sender.session.add_stream(feed()));
    hub.deliver_all();
    subscribe(receiver, 4);
    hub.lose = [](const Message& m) { return number_of(m) == 2; };
    for (FrameNumber n = 0; n < 6; ++n) ASSERT_TRUE(send(n, true, 4));
    hub.reverse_live();
    advance(1s);
    EXPECT_EQ(receiver.frames, (Frames{5, 4, 3, 1, 0}));
    EXPECT_EQ(sender.keyframes_needed, 0);
    for (uint64_t tag : sender.endpoint.tags) EXPECT_EQ(tag, 0u);
    for (LiveRank rank : sender.endpoint.ranks) EXPECT_EQ(rank, LiveRank::Frame);
}

// ── Joining ─────────────────────────────────────────────────────────────────

// A fresh connection lets a keyframe through only over a few round trips: the
// first one a subscriber gets may come late rather than not at all, and the
// frames behind it as well, each still due after the one before it.
TEST_F(MediaSessionTest, AJoiningKeyframeAndTheFramesBehindItMayArriveLate) {
    sender.session.on_peer_path(receiver.id, path());  // a 50 ms round trip
    subscribe(receiver);
    ASSERT_TRUE(send(0, false));  // not for it: it waits for a keyframe
    for (FrameNumber n = 1; n <= 50; ++n) {
        ASSERT_TRUE(send(n, n == 1 || n == 30));
        clock.advance(10ms);
    }
    const std::vector<Duration>& lifetimes = sender.endpoint.lifetimes;
    ASSERT_EQ(lifetimes.size(), 50u);
    EXPECT_EQ(lifetimes[0], 150ms + 4 * 50ms);
    EXPECT_EQ(lifetimes[1], 345ms);  // half the 10 ms since
    for (size_t i = 1; i < lifetimes.size(); ++i) {
        EXPECT_GE(Duration{10ms} + lifetimes[i], lifetimes[i - 1]) << "frame " << i + 1;
    }
    EXPECT_EQ(lifetimes[29], 205ms) << "a later keyframe is no join";
    EXPECT_EQ(lifetimes.back(), 150ms);  // the stream's own
    hub.deliver_all();
    EXPECT_EQ(receiver.frames.size(), 50u);
}

TEST_F(MediaSessionTest, WhileItsKeyframeIsOnItsWayAJoiningReceiverAsksInVain) {
    subscribe(receiver);
    hub.hold = [](const Message&) { return true; };
    ASSERT_TRUE(send(0, true));
    advance(300ms);  // nothing came: the receiver asks for a keyframe
    EXPECT_EQ(sender.session.outgoing_stats(1)->keyframe_requests, 1u);
    EXPECT_EQ(sender.keyframes_needed, 1);  // the one subscribing asked for

    hub.release();
    EXPECT_EQ(receiver.frames, Frames{0});
    receiver.session.request_keyframe(sender.id, 1);  // its decoder failed, say
    hub.deliver_all();
    EXPECT_EQ(sender.keyframes_needed, 2);
}

TEST_F(MediaSessionTest, AJoiningKeyframeGivenUpOnIsSentAgain) {
    sender.session.on_peer_path(receiver.id, path());
    subscribe(receiver);
    hub.lose = [](const Message& m) { return number_of(m) == 0; };
    ASSERT_TRUE(send(0, true));
    hub.deliver_all();
    advance(200ms);  // min_keyframe_interval after the one subscribing asked for
    EXPECT_EQ(sender.keyframes_needed, 2);
    ASSERT_TRUE(send(1, false));  // of no use without frame 0: not sent
    ASSERT_TRUE(send(2, true));
    hub.deliver_all();
    EXPECT_EQ(receiver.frames, Frames{2});
    EXPECT_EQ(sender.endpoint.lifetimes, (std::vector<Duration>{350ms, 350ms}));
}

// A frame behind a joining keyframe still on its way is given up on: the
// keyframe asked for now goes right behind it, and is all that is needed.
TEST_F(MediaSessionTest, AFrameLostBehindAJoiningKeyframeIsAKeyframeNeededAtOnce) {
    sender.session.on_peer_path(receiver.id, path());
    subscribe(receiver);
    hub.hold = [](const Message& m) { return number_of(m) == 0; };
    hub.lose = [](const Message& m) { return number_of(m) == 1; };
    ASSERT_TRUE(send(0, true));
    ASSERT_TRUE(send(1, false));
    hub.deliver_all();
    EXPECT_EQ(sender.keyframes_needed, 1);  // held back: a keyframe was just sent...
    advance(200ms);
    EXPECT_EQ(sender.keyframes_needed, 2);  // ...until min_keyframe_interval has passed
    ASSERT_TRUE(send(2, true));
    EXPECT_EQ(sender.endpoint.lifetimes.back(), 250ms) << "not behind the joining keyframe";

    hub.release();
    advance(1s);
    EXPECT_EQ(sender.keyframes_needed, 2);
    EXPECT_EQ(receiver.frames, (Frames{0, 2}));
}

TEST_F(MediaSessionTest, KeyframeRequestsFromManyReceiversAreCoalesced) {
    Party other(hub, clock, 3);
    connect(sender, other);
    hub.deliver_all();
    subscribe(receiver);
    subscribe(other);
    EXPECT_EQ(sender.keyframes_needed, 1);  // the second subscriber's need is held back

    advance(100ms);
    EXPECT_EQ(sender.keyframes_needed, 1);
    advance(100ms);  // min_keyframe_interval has passed
    EXPECT_EQ(sender.keyframes_needed, 2);

    ASSERT_TRUE(send(0, true));
    hub.deliver_all();
    receiver.session.request_keyframe(sender.id, 1);  // its decoder failed on frame 0, say
    hub.deliver_all();
    EXPECT_EQ(sender.keyframes_needed, 2);  // ignored: too soon after a keyframe
    advance(250ms);                         // the receiver asks again, and now it counts
    EXPECT_EQ(sender.keyframes_needed, 3);

    other.session.request_keyframe(sender.id, 1);
    hub.deliver_all();
    EXPECT_EQ(sender.keyframes_needed, 3);  // held back: one was just asked for...
    ASSERT_TRUE(send(1, true));
    advance(1s);
    EXPECT_EQ(sender.keyframes_needed, 3);  // ...and the keyframe answers both
}

TEST_F(MediaSessionTest, AReceiverThatAsksForAKeyframeDropsWhatCannotDecode) {
    subscribe(receiver);
    ASSERT_TRUE(send(0, true));
    ASSERT_TRUE(send(1, false));
    hub.deliver_all();
    receiver.session.request_keyframe(sender.id, 1);
    ASSERT_TRUE(send(2, false));  // depends on what the decoder lost
    ASSERT_TRUE(send(3, true));
    ASSERT_TRUE(send(4, false));
    hub.deliver_all();
    EXPECT_EQ(receiver.frames, (Frames{0, 1, 3, 4}));
}

TEST_F(MediaSessionTest, AFullQueueCostsTheSubscriberFramesUntilAKeyframe) {
    subscribe(receiver);
    ASSERT_TRUE(send(0, true));
    advance(1s);

    sender.endpoint.set_full(receiver.id, true);
    ASSERT_TRUE(send(1, false));            // queued, but the queue is now full
    ASSERT_TRUE(send(2, false));            // dropped
    ASSERT_TRUE(send(3, false));            // dropped too, and useless without 2 anyway
    advance(1s);                            // still full when tick() polls it
    EXPECT_EQ(sender.keyframes_needed, 1);  // no keyframe into a full queue

    sender.endpoint.set_full(receiver.id, false);
    sender.session.on_peer_writable(receiver.id);
    ASSERT_TRUE(send(4, false));  // the subscriber can take frames again: it needs a keyframe
    EXPECT_EQ(sender.keyframes_needed, 2);
    ASSERT_TRUE(send(5, true));
    hub.deliver_all();

    EXPECT_EQ(receiver.frames, (Frames{0, 1, 5}));
    const auto stats = sender.session.outgoing_stats(1);
    EXPECT_EQ(stats->backpressure_drops, 1u);  // frame 2
    EXPECT_EQ(stats->awaiting_keyframe, 2u);   // frames 3 and 4
}

TEST_F(MediaSessionTest, ABlockedPeerIsPolledWhenTheEventNeverComes) {
    subscribe(receiver);
    ASSERT_TRUE(send(0, true));
    sender.endpoint.set_full(receiver.id, true);
    ASSERT_TRUE(send(1, false));
    sender.endpoint.set_full(receiver.id, false);
    advance(1s);  // tick() finds the peer writable again

    ASSERT_TRUE(send(2, true));
    hub.deliver_all();
    EXPECT_EQ(receiver.frames, (Frames{0, 1, 2}));
}

// ── Rate control ────────────────────────────────────────────────────────────

TEST_F(MediaSessionTest, RefusesBitrateLimitsThatAreNotValid) {
    EXPECT_EQ(sender.session.add_stream(screen(2), BitrateLimits{1, 3, 2}).error(),
              Errc::InvalidArgument);
}

TEST_F(MediaSessionTest, RateControlFollowsTheSubscribersPath) {
    add_rated_stream();
    subscribe(receiver, 2);
    EXPECT_EQ(sender.session.outgoing_stats(2)->bitrate, kBitrate.start);

    sender.session.on_peer_path(receiver.id, path());  // the first sample starts the clock
    clock.advance(1s);
    sender.session.on_peer_path(receiver.id, path());
    EXPECT_EQ(sender.bitrates, Bitrates{3'000'000});

    clock.advance(100ms);
    sender.session.on_peer_path(receiver.id, path(1));  // a frame expired
    EXPECT_EQ(sender.bitrates, (Bitrates{3'000'000, 2'550'000}));

    const auto stats = sender.session.outgoing_stats(2);
    EXPECT_EQ(stats->bitrate, 2'550'000u);
    EXPECT_EQ(stats->bitrate_raises, 1u);
    EXPECT_EQ(stats->bitrate_cuts, 1u);
    EXPECT_EQ(sender.session.outgoing_stats(1)->bitrate, 0u);  // not rate-controlled
}

TEST_F(MediaSessionTest, RateControlHearsOnlyItsSubscribersPaths) {
    add_rated_stream();
    sender.session.on_peer_path(receiver.id, path(1));
    EXPECT_TRUE(sender.bitrates.empty());
}

TEST_F(MediaSessionTest, FramesExpiringWhileTheTransportStartsAreNoOveruse) {
    add_rated_stream();
    subscribe(receiver, 2);
    sender.session.on_peer_path(receiver.id, starting_path(1));  // the joining keyframe
    EXPECT_TRUE(sender.bitrates.empty());

    clock.advance(100ms);
    sender.session.on_peer_path(receiver.id, path(2));
    EXPECT_EQ(sender.bitrates, Bitrates{1'700'000});
}

TEST_F(MediaSessionTest, AFullQueueIsOveruse) {
    add_rated_stream();
    subscribe(receiver, 2);
    sender.endpoint.set_full(receiver.id, true);
    ASSERT_TRUE(send(0, true, 2));  // queued, but the queue is now full
    sender.session.on_peer_path(receiver.id, path());
    EXPECT_EQ(sender.bitrates, Bitrates{1'700'000});
}

/// A path of 2 Mbit/s the sender keeps busy, `bytes` delivered and
/// `expired` live messages expired since it opened.
PathStats busy_path(uint64_t bytes, uint64_t expired = 0) {
    PathStats s = path(expired);
    s.bandwidth_bps = 2'000'000;
    s.app_limited = false;
    s.bytes_delivered = bytes;
    return s;
}

/// Stream 3 beside stream 2: a camera, at most a quarter of the screen.
constexpr BitrateLimits kCamera{.min = 100'000, .start = 500'000, .max = 2'000'000};

TEST_F(MediaSessionTest, StreamsToOnePeerShareItsPath) {
    add_rated_stream();
    ASSERT_TRUE(sender.session.add_stream(screen(3), kCamera));
    hub.deliver_all();
    subscribe(receiver, 2);
    subscribe(receiver, 3);

    // 2 Mbit/s delivered, and frames expired: together they come down to
    // 85% of it, the screen with four fifths, the camera with one.
    sender.session.on_peer_path(receiver.id, busy_path(0));
    clock.advance(100ms);
    sender.session.on_peer_path(receiver.id, busy_path(25'000, 1));
    EXPECT_EQ(sender.session.outgoing_stats(2)->bitrate, 1'360'000u);
    EXPECT_EQ(sender.session.outgoing_stats(3)->bitrate, 340'000u);
}

TEST_F(MediaSessionTest, AStreamThePeerDoesNotWatchTakesNoPartOfItsPath) {
    add_rated_stream();
    ASSERT_TRUE(sender.session.add_stream(screen(3), kCamera));
    hub.deliver_all();
    subscribe(receiver, 2);

    sender.session.on_peer_path(receiver.id, busy_path(0));
    clock.advance(100ms);
    sender.session.on_peer_path(receiver.id, busy_path(25'000, 1));
    EXPECT_EQ(sender.session.outgoing_stats(2)->bitrate, 1'700'000u);
    EXPECT_EQ(sender.session.outgoing_stats(3)->bitrate, kCamera.start);
}

/// A path the sender does not fill, wide on its own, that carried `bytes`
/// since it opened: one of several through the sender's uplink.
PathStats carried(uint64_t bytes, uint64_t expired = 0) {
    PathStats s = path(expired);
    s.bytes_delivered = bytes;
    return s;
}

TEST_F(MediaSessionTest, RateControlRampsPastWhereThePathFilledUpOnlyOnceItCarriedWellPast) {
    add_rated_stream();
    subscribe(receiver, 2);
    sender.session.on_peer_path(receiver.id, busy_path(0));
    clock.advance(100ms);
    sender.session.on_peer_path(receiver.id, busy_path(25'000, 1));  // full at 2 Mbit/s

    // The encoder sends 1 Mbit/s of it: carefully on, +8% a step.
    uint64_t bytes = 25'000;
    for (int second = 0; second < 5; ++second) {
        clock.advance(1s);
        bytes += 125'000;
        sender.session.on_peer_path(receiver.id, carried(bytes));
    }
    EXPECT_EQ(sender.bitrates, (Bitrates{1'700'000, 1'836'000, 1'982'880, 2'141'510, 2'312'831}));

    // 2.4 Mbit/s got through: the path has grown.
    clock.advance(1s);
    bytes += 300'000;
    sender.session.on_peer_path(receiver.id, carried(bytes));
    EXPECT_EQ(sender.bitrates.back(), 3'469'247u);
}

TEST_F(MediaSessionTest, CopiesThroughAFullUplinkShareIt) {
    Party other(hub, clock, 3);
    connect(sender, other);
    add_rated_stream();
    subscribe(receiver, 2);
    subscribe(other, 2);

    // Each path wide on its own, but both lose frames at once with 3 Mbit/s
    // through each: the uplink they share is full at 6.
    for (Party* to : {&receiver, &other}) sender.session.on_peer_path(to->id, carried(0));
    clock.advance(100ms);
    sender.session.on_peer_path(receiver.id, carried(37'500, 1));
    EXPECT_FALSE(sender.session.uplink_capacity());  // one path alone is its own
    sender.session.on_peer_path(other.id, carried(37'500, 1));
    EXPECT_EQ(sender.session.uplink_capacity(), 6'000'000u);
    EXPECT_EQ(sender.bitrates, Bitrates{1'700'000});
    // Each path sent no faster than its half of it, bursts and all.
    using Ceilings = std::map<PeerId, uint64_t>;
    EXPECT_EQ(sender.endpoint.ceilings,
              (Ceilings{{receiver.id, 3'000'000}, {other.id, 3'000'000}}));

    // Climbing again, no higher than its part: 85% of 3 Mbit/s a copy, less
    // the 10% any raise leaves.
    uint64_t bytes = 37'500;
    for (int second = 0; second < 4; ++second) {
        clock.advance(1s);
        bytes += 150'000;
        for (Party* to : {&receiver, &other})
            sender.session.on_peer_path(to->id, carried(bytes, 1));
    }
    EXPECT_EQ(sender.bitrates, (Bitrates{1'700'000, 2'295'000}));

    // A third viewer: three copies through it, down before its keyframe.
    Party third(hub, clock, 4);
    connect(sender, third);
    hub.deliver_all();
    subscribe(third, 2);
    EXPECT_EQ(sender.bitrates, (Bitrates{1'700'000, 2'295'000, 1'530'000}));
    // And each path to a third of the uplink, as its samples come.
    sender.session.on_peer_path(third.id, carried(0));
    EXPECT_EQ(sender.endpoint.ceilings,
              (Ceilings{{receiver.id, 2'000'000}, {other.id, 2'000'000}, {third.id, 2'000'000}}));

    // A minute after it was found full, the uplink is forgotten, and so are
    // the ceilings: rate control finds it again if it is still there.
    clock.advance(SharedUplink::kMemory + 1s);
    sender.session.on_peer_path(receiver.id, carried(bytes));
    EXPECT_FALSE(sender.session.uplink_capacity());
    EXPECT_TRUE(sender.endpoint.ceilings.empty());
}

TEST_F(MediaSessionTest, CopiesComeDownAsTheUplinksQueueBuildsBeforeItOverflows) {
    Party other(hub, clock, 3);
    connect(sender, other);
    add_rated_stream();
    subscribe(receiver, 2);
    subscribe(other, 2);

    // 2 Mbit/s through each, and the round trips rising together, two
    // samples running: 4 Mbit/s fill the uplink, nothing expired yet.
    uint64_t bytes = 0;
    for (uint32_t rtt : {50'000u, 80'000u, 85'000u}) {
        for (Party* to : {&receiver, &other}) {
            PathStats s = carried(bytes);
            s.srtt_us = rtt;
            sender.session.on_peer_path(to->id, s);
        }
        clock.advance(100ms);
        bytes += 25'000;
    }
    EXPECT_EQ(sender.session.uplink_capacity(), 4'000'000u);
    // Each copy at once to its part: 85% of 2 Mbit/s, less 10%.
    EXPECT_EQ(sender.bitrates, Bitrates{1'530'000});
}

TEST_F(MediaSessionTest, AnUplinkIsNotFullWhenOnlySomeOfItsPathsAre) {
    Party other(hub, clock, 3);
    connect(sender, other);
    add_rated_stream();
    subscribe(receiver, 2);
    subscribe(other, 2);
    for (Party* to : {&receiver, &other}) sender.session.on_peer_path(to->id, carried(0));
    clock.advance(100ms);
    sender.session.on_peer_path(receiver.id, carried(37'500, 1));
    sender.session.on_peer_path(other.id, carried(37'500));
    EXPECT_FALSE(sender.session.uplink_capacity());
}

TEST_F(MediaSessionTest, AStreamNobodyWatchesStartsAfresh) {
    add_rated_stream();
    subscribe(receiver, 2);
    sender.session.on_peer_path(receiver.id, path(1));
    receiver.session.unsubscribe(sender.id, 2);
    hub.deliver_all();
    EXPECT_EQ(sender.bitrates, (Bitrates{1'700'000, kBitrate.start}));

    subscribe(receiver, 2);
    sender.session.on_peer_path(receiver.id, path(2));
    sender.session.on_peer_disconnected(receiver.id);
    EXPECT_EQ(sender.bitrates, (Bitrates{1'700'000, kBitrate.start, 1'700'000, kBitrate.start}));
    EXPECT_EQ(sender.session.outgoing_stats(2)->bitrate_cuts, 2u);  // the restarts are not
    EXPECT_EQ(sender.session.outgoing_stats(2)->bitrate_raises, 0u);
}

// ── Who is who ──────────────────────────────────────────────────────────────

TEST_F(MediaSessionTest, APresenceIsToldToEveryPeerNowAndAsTheyConnect) {
    const Presence ann{"Ann", true, false};
    ASSERT_TRUE(sender.session.set_presence(ann));
    hub.deliver_all();
    EXPECT_EQ(receiver.presences, std::vector{ann});
    EXPECT_EQ(receiver.session.presence(sender.id), ann);
    EXPECT_FALSE(sender.session.presence(receiver.id)) << "the receiver said nothing";

    Party late(hub, clock, 3);
    connect(sender, late);
    hub.deliver_all();
    EXPECT_EQ(late.session.presence(sender.id), ann);

    // A change, to everyone.
    const Presence unmuted{"Ann", false, false};
    ASSERT_TRUE(sender.session.set_presence(unmuted));
    hub.deliver_all();
    EXPECT_EQ(receiver.session.presence(sender.id), unmuted);
    EXPECT_EQ(late.session.presence(sender.id), unmuted);
    EXPECT_EQ(receiver.presences.size(), 2u);
}

TEST_F(MediaSessionTest, APresenceIsToldAgainWhenALinkLostARace) {
    ASSERT_TRUE(sender.session.set_presence({"Ann"}));
    hub.queue.clear();  // went with the link that lost
    sender.session.on_peer_rerouted(receiver.id);
    hub.deliver_all();
    EXPECT_EQ(receiver.session.presence(sender.id), Presence{"Ann"});
}

TEST_F(MediaSessionTest, APresenceIsForgottenWithItsPeer) {
    ASSERT_TRUE(sender.session.set_presence({"Ann"}));
    hub.deliver_all();
    receiver.session.on_peer_disconnected(sender.id);
    EXPECT_FALSE(receiver.session.presence(sender.id));
}

TEST_F(MediaSessionTest, RefusesAPresenceThatIsNotValid) {
    EXPECT_EQ(sender.session.set_presence({"a\nb"}).error(), Errc::InvalidArgument);
    hub.deliver_all();
    EXPECT_TRUE(receiver.presences.empty());
}

// ── Peers coming and going ──────────────────────────────────────────────────

TEST_F(MediaSessionTest, ADisconnectEndsStreamsAndSubscriptions) {
    subscribe(receiver);
    receiver.session.on_peer_disconnected(sender.id);
    EXPECT_EQ(receiver.withdrawn, std::vector<StreamId>{1});
    EXPECT_FALSE(receiver.session.incoming_stats(sender.id, 1));

    sender.session.on_peer_disconnected(receiver.id);
    EXPECT_EQ(sender.session.outgoing_stats(1)->subscribers, 0u);
}

TEST_F(MediaSessionTest, WhatALinkThatLostARaceTookIsSaidAgain) {
    // A stream offered and a subscription asked for on a link that then lost a
    // cross-connect race: neither arrives.
    ASSERT_TRUE(sender.session.add_stream(screen(2)));
    ASSERT_TRUE(receiver.session.subscribe(sender.id, 1));
    hub.queue.clear();
    ASSERT_EQ(receiver.announced.size(), 1u);
    ASSERT_EQ(sender.session.outgoing_stats(1)->subscribers, 0u);

    sender.session.on_peer_rerouted(receiver.id);
    receiver.session.on_peer_rerouted(sender.id);
    hub.deliver_all();
    ASSERT_EQ(receiver.announced.size(), 3u);  // the first again, and the second
    EXPECT_NE(std::ranges::find(receiver.announced, screen(2)), receiver.announced.end());
    EXPECT_EQ(sender.session.outgoing_stats(1)->subscribers, 1u);
    EXPECT_EQ(sender.session.outgoing_stats(2)->subscribers, 0u) << "only what was asked for";

    // What did arrive, arriving twice, changes nothing.
    receiver.session.on_peer_rerouted(sender.id);
    hub.deliver_all();
    EXPECT_EQ(sender.session.outgoing_stats(1)->subscribers, 1u);
}

TEST_F(MediaSessionTest, CountsMessagesItCannotRead) {
    const uint8_t garbage[] = {0xFF, 0x00};
    receiver.session.on_message(sender.id, garbage);
    const uint8_t truncated_frame[] = {wire::message_prefix(wire::MessageKind::Frame)};
    receiver.session.on_message(sender.id, truncated_frame);
    EXPECT_EQ(receiver.session.rejected_messages(), 2u);
}

TEST_F(MediaSessionTest, HandlersMayCallBackIntoTheSession) {
    subscribe(receiver);
    receiver.on_frame = [&](const EncodedFrame& f) {
        receiver.session.request_keyframe(sender.id, f.info.stream);
        receiver.session.unsubscribe(sender.id, f.info.stream);
    };
    ASSERT_TRUE(send(0, true));
    hub.deliver_all();
    EXPECT_EQ(receiver.frames, Frames{0});
    EXPECT_EQ(sender.session.outgoing_stats(1)->subscribers, 0u);
}

// ── Playing in step ─────────────────────────────────────────────────────────

namespace {

/// Another host's clock: `base`'s, `ahead` of it.
class OffsetClock final : public Clock {
public:
    OffsetClock(const Clock& base, Duration ahead) : base_(base), ahead_(ahead) {}
    Timestamp now() const noexcept override { return base_.now() + ahead_; }

private:
    const Clock& base_;
    const Duration ahead_;
};

/// The sender's frames are captured on a clock an hour ahead of its session's.
constexpr Duration kCaptureAhead = 1h;
/// The viewers' clocks, against the sender's session's: no two hosts agree.
constexpr Duration kNearAhead = 1000s;
constexpr Duration kFarAhead = -7s;
/// How far each is from the sender, one way.
constexpr Duration kNearWay = 10ms;
constexpr Duration kFarWay = 50ms;

/// A sender and two viewers, each on a clock of its own.
class InStepTest : public ::testing::Test {
protected:
    InStepTest() {
        for (Party* viewer : {&near, &far}) {
            viewer->session.on_peer_connected(sender.id);
            sender.session.on_peer_connected(viewer->id);
        }
        hub.deliver_all();
    }

    /// `viewer` needs `latency` from a capture to playing it, as the sender's
    /// capture clock sees it: so much more on its own clock. Its probe goes,
    /// is answered, and what it needs told, `one_way` each hop.
    void need(Party& viewer, Duration latency) {
        viewer.session.set_playout_need(sender.id, latency + ahead(viewer));
        for (int hop = 0; hop < 3; ++hop) {
            clock.advance(&viewer == &near ? kNearWay : kFarWay);
            hub.deliver_queued();
        }
        hub.deliver_all();  // the delay, if it changed: when it comes does not matter
    }

    /// The viewer's clock less the sender's capture clock.
    Duration ahead(const Party& viewer) const {
        return (&viewer == &near ? kNearAhead : kFarAhead) - kCaptureAhead;
    }

    std::optional<CommonPlayout> playout(const Party& viewer) const {
        return viewer.session.common_playout(sender.id);
    }

    /// Let `d` pass, a tick every 10 ms.
    void advance(Duration d) {
        for (Duration passed{}; passed < d; passed += 10ms) {
            clock.advance(10ms);
            for (Party* party : {&sender, &near, &far}) party->session.tick();
            hub.deliver_all();
        }
    }

    ManualClock clock{Timestamp{100s}};
    OffsetClock capture{clock, kCaptureAhead};
    OffsetClock near_clock{clock, kNearAhead};
    OffsetClock far_clock{clock, kFarAhead};
    Hub hub;
    Party sender{hub, clock, 1, SessionConfig{.capture_clock = &capture}};
    Party near{hub, near_clock, 2};
    Party far{hub, far_clock, 3};
};

}  // namespace

TEST_F(InStepTest, ViewersPlayAShareAtTheMostAnyOfThemNeeds) {
    need(near, 60ms);
    need(far, 150ms);
    const Duration delay = 150ms + CommonDelay::kMargin;
    ASSERT_TRUE(playout(near));
    ASSERT_TRUE(playout(far));
    EXPECT_EQ(playout(near)->delay, delay);
    EXPECT_EQ(playout(far)->delay, delay);
    // Each on its own clock — the same moment.
    EXPECT_EQ(playout(near)->offset, delay + ahead(near));
    EXPECT_EQ(playout(far)->offset, delay + ahead(far));
}

TEST_F(InStepTest, AViewerAloneKeepsItsOwnTime) {
    need(near, 60ms);
    EXPECT_FALSE(playout(near));
    EXPECT_EQ(hub.delivered[wire::MessageKind::PlayoutDelay], 0);
}

TEST_F(InStepTest, AViewerThatStopsOrLeavesNoLongerCounts) {
    need(near, 60ms);
    need(far, 150ms);
    far.session.set_playout_need(sender.id, std::nullopt);
    hub.deliver_all();
    EXPECT_FALSE(playout(near)) << "alone again";

    need(far, 150ms);
    ASSERT_TRUE(playout(near));
    sender.session.on_peer_disconnected(far.id);
    hub.deliver_all();
    EXPECT_FALSE(playout(near));
}

TEST_F(InStepTest, ANeedIsToldAsItChanges) {
    need(near, 60ms);
    need(far, 150ms);
    const int told = hub.delivered[wire::MessageKind::PlayoutNeed];
    far.session.set_playout_need(sender.id, 153ms + ahead(far));
    hub.deliver_all();
    EXPECT_EQ(hub.delivered[wire::MessageKind::PlayoutNeed], told) << "no news";

    far.session.set_playout_need(sender.id, 300ms + ahead(far));
    hub.deliver_all();
    EXPECT_EQ(hub.delivered[wire::MessageKind::PlayoutNeed], told + 1);
    EXPECT_EQ(playout(near)->delay, 300ms + CommonDelay::kMargin) << "up at once";

    // Down only once it stayed down.
    far.session.set_playout_need(sender.id, 150ms + ahead(far));
    hub.deliver_all();
    advance(CommonDelay::kLowerAfter - 100ms);
    EXPECT_EQ(playout(near)->delay, 300ms + CommonDelay::kMargin);
    advance(200ms);
    EXPECT_EQ(playout(near)->delay, 150ms + CommonDelay::kMargin);
}

TEST_F(InStepTest, AClockIsProbedOftenAtFirstThenEverySecondUntilNoLongerPlayed) {
    near.session.set_playout_need(sender.id, 60ms + ahead(near));
    advance(1s);
    EXPECT_EQ(hub.delivered[wire::MessageKind::ClockProbe], 5);
    advance(2s);
    EXPECT_EQ(hub.delivered[wire::MessageKind::ClockProbe], 7);
    EXPECT_EQ(hub.delivered[wire::MessageKind::ClockEcho], 7);

    near.session.set_playout_need(sender.id, std::nullopt);
    advance(3s);
    EXPECT_EQ(hub.delivered[wire::MessageKind::ClockProbe], 7);
}

TEST_F(InStepTest, NeedAndDelayAreSaidAgainWhenALinkLostARace) {
    need(near, 60ms);
    need(far, 150ms);
    const int needs = hub.delivered[wire::MessageKind::PlayoutNeed];
    const int delays = hub.delivered[wire::MessageKind::PlayoutDelay];
    near.session.on_peer_rerouted(sender.id);
    sender.session.on_peer_rerouted(near.id);
    hub.deliver_all();
    EXPECT_EQ(hub.delivered[wire::MessageKind::PlayoutNeed], needs + 1);
    EXPECT_EQ(hub.delivered[wire::MessageKind::PlayoutDelay], delays + 1);
    EXPECT_EQ(playout(near)->delay, 150ms + CommonDelay::kMargin);
}

// ── Relaying ────────────────────────────────────────────────────────────────

namespace {

/// A sender, a viewer of its screen that relays it, and one it passes it on
/// to, all connected to each other as a room is; both watch the screen.
class RelayTest : public ::testing::Test {
protected:
    RelayTest() {
        connect(sender, relay);
        connect(sender, viewer);
        connect(relay, viewer);
        EXPECT_TRUE(sender.session.add_stream(screen(2), kBitrate));
        EXPECT_TRUE(sender.session.add_stream(mic()));
        hub.deliver_all();
        watch(relay);
        watch(viewer);
    }

    void connect(Party& a, Party& b) {
        a.session.on_peer_connected(b.id);
        b.session.on_peer_connected(a.id);
    }

    void watch(Party& who, StreamId stream = 2) {
        ASSERT_TRUE(who.session.subscribe(sender.id, stream));
        hub.deliver_all();
    }

    Result<void> send(FrameNumber number, bool keyframe, StreamId stream = 2) {
        FrameInfo info;
        info.number = number;
        info.keyframe = keyframe;
        info.capture_time = clock.now();
        if (!keyframe && number > 0) info.references = {number - 1};
        return sender.session.send_frame(stream, info, payload_for(number));
    }

    /// Live messages from `from` to `to` on their way now.
    size_t live(const Party& from, const Party& to) const {
        return static_cast<size_t>(std::ranges::count_if(hub.queue, [&](const Message& m) {
            return m.live && m.from == from.id && m.to == to.id;
        }));
    }

    ManualClock clock{Timestamp{100s}};
    Hub hub;
    Party sender{hub, clock, 1};
    Party relay{hub, clock, 2};
    Party viewer{hub, clock, 3};
};

}  // namespace

TEST_F(RelayTest, AViewersFramesGoThroughItsRelayFromAKeyframeOn) {
    ASSERT_TRUE(send(0, true));
    hub.deliver_all();
    EXPECT_EQ(viewer.frames, Frames{0});

    ASSERT_TRUE(sender.session.relay_through(2, viewer.id, relay.id));
    hub.deliver_all();
    EXPECT_EQ(hub.delivered[wire::MessageKind::Forward], 1);
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 1u);

    // The relay passes the stream on from its next keyframe, which it asks
    // the sender for.
    clock.advance(300ms);
    const int asked = sender.keyframes_needed;
    ASSERT_TRUE(send(1, false));
    EXPECT_EQ(live(sender, viewer), 0u);
    EXPECT_EQ(live(sender, relay), 1u);
    hub.deliver_all();
    EXPECT_EQ(sender.keyframes_needed, asked + 1);

    ASSERT_TRUE(send(2, true));
    ASSERT_TRUE(send(3, false));
    EXPECT_EQ(live(sender, viewer), 0u) << "not a copy through the sender's uplink";
    hub.deliver_queued();  // to the relay
    ASSERT_EQ(live(relay, viewer), 2u);
    for (const Message& m : hub.queue) {
        EXPECT_EQ(*wire::peek_kind(m.bytes.bytes()), wire::MessageKind::RelayedFrame);
    }
    hub.deliver_all();
    EXPECT_EQ(viewer.frames, (Frames{0, 2, 3}));
    EXPECT_EQ(relay.frames, (Frames{0, 1, 2, 3}));

    const IncomingStreamStats stats = *relay.session.incoming_stats(sender.id, 2);
    EXPECT_EQ(stats.relaying_to, 1u);
    EXPECT_EQ(stats.relayed_messages, 2u);
    EXPECT_GT(stats.relayed_bytes, 0u);
}

TEST_F(RelayTest, AFramePassedOnHasWhatIsLeftOfItsTime) {
    watch(relay, 3);
    watch(viewer, 3);
    ASSERT_TRUE(sender.session.relay_through(3, viewer.id, relay.id));
    hub.deliver_all();

    // The sound's lifetime is 100 ms; it reached the relay in 40.
    ASSERT_TRUE(send(0, true, 3));
    clock.advance(40ms);
    hub.deliver_all();
    ASSERT_EQ(relay.endpoint.lifetimes.size(), 1u);
    EXPECT_EQ(relay.endpoint.lifetimes.back(), 60ms);
    EXPECT_EQ(viewer.frames, Frames{0});

    // One that came too late for the viewer goes no further.
    ASSERT_TRUE(send(1, true, 3));
    clock.advance(120ms);
    hub.deliver_all();
    EXPECT_EQ(relay.endpoint.lifetimes.size(), 1u);
    EXPECT_EQ(relay.frames, (Frames{0, 1}));
    EXPECT_EQ(relay.session.incoming_stats(sender.id, 3)->relay_held_back, 0u)
        << "a frame of sound costs only itself";

    // Where the network does not say (TCP): its lifetime again, less half the
    // round trip it came over.
    relay.session.on_peer_path(sender.id, path());  // a 50 ms round trip
    ASSERT_TRUE(send(2, true, 3));
    Message m = std::move(hub.queue.back());
    hub.queue.pop_back();
    relay.session.on_message(m.from, m.bytes.bytes(), std::nullopt);
    EXPECT_EQ(relay.endpoint.lifetimes.back(), 75ms);
}

TEST_F(RelayTest, AFrameLostOnItsWayFromTheRelayIsAKeyframeNeeded) {
    ASSERT_TRUE(sender.session.relay_through(2, viewer.id, relay.id));
    hub.deliver_all();
    ASSERT_TRUE(send(0, true));
    ASSERT_TRUE(send(1, false));
    hub.deliver_all();
    EXPECT_EQ(viewer.frames, (Frames{0, 1}));

    clock.advance(300ms);
    const int asked = sender.keyframes_needed;
    hub.lose = [&](const Message& m) { return m.from == relay.id; };
    ASSERT_TRUE(send(2, false));
    hub.deliver_all();
    EXPECT_EQ(sender.keyframes_needed, asked + 1) << "asked by the relay as the network said";
    EXPECT_EQ(relay.frames, (Frames{0, 1, 2}));
}

TEST_F(RelayTest, RateControlHearsAViewersPathFromItsRelay) {
    ASSERT_TRUE(sender.session.relay_through(2, viewer.id, relay.id));
    hub.deliver_all();
    relay.session.on_peer_path(viewer.id, path());
    clock.advance(100ms);
    relay.session.on_peer_path(viewer.id, path(1));  // a frame expired
    hub.deliver_all();
    EXPECT_EQ(hub.delivered[wire::MessageKind::RelayedPath], 2);
    EXPECT_EQ(sender.bitrates, Bitrates{1'700'000});

    // Not from the sender's own path to the viewer: no copy of it goes there.
    clock.advance(2s);
    sender.session.on_peer_path(viewer.id, path(1));
    clock.advance(100ms);
    sender.session.on_peer_path(viewer.id, path(5));
    EXPECT_EQ(sender.bitrates, Bitrates{1'700'000});
}

TEST_F(RelayTest, ARelaysUplinkCarriesWhatItPassesOn) {
    Party other(hub, clock, 4);
    connect(sender, other);
    connect(relay, other);
    hub.deliver_all();
    watch(other);
    ASSERT_TRUE(sender.session.relay_through(2, viewer.id, relay.id));
    ASSERT_TRUE(sender.session.relay_through(2, other.id, relay.id));
    hub.deliver_all();
    EXPECT_EQ(relay.session.incoming_stats(sender.id, 2)->relaying_to, 2u);

    // Both its paths lose frames at once with 3 Mbit/s through each.
    for (Party* to : {&viewer, &other}) relay.session.on_peer_path(to->id, carried(0));
    clock.advance(100ms);
    for (Party* to : {&viewer, &other}) relay.session.on_peer_path(to->id, carried(37'500, 1));
    EXPECT_EQ(relay.session.uplink_capacity(), 6'000'000u);
    using Ceilings = std::map<PeerId, uint64_t>;
    EXPECT_EQ(relay.endpoint.ceilings, (Ceilings{{viewer.id, 3'000'000}, {other.id, 3'000'000}}));
    EXPECT_FALSE(sender.session.uplink_capacity()) << "not the sender's";
}

TEST_F(RelayTest, ARelayThatStopsWatchingLeavesItsViewersToTheSender) {
    ASSERT_TRUE(sender.session.relay_through(2, viewer.id, relay.id));
    hub.deliver_all();
    clock.advance(300ms);
    const int asked = sender.keyframes_needed;
    relay.session.unsubscribe(sender.id, 2);
    hub.deliver_all();
    EXPECT_EQ(sender.keyframes_needed, asked + 1);
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 0u);
    ASSERT_TRUE(send(0, true));
    EXPECT_EQ(live(sender, viewer), 1u);
    hub.deliver_all();
    EXPECT_EQ(viewer.frames, Frames{0});
}

TEST_F(RelayTest, ARelayThatGoesAwayLeavesItsViewersToTheSender) {
    ASSERT_TRUE(sender.session.relay_through(2, viewer.id, relay.id));
    hub.deliver_all();
    sender.session.on_peer_disconnected(relay.id);
    viewer.session.on_peer_disconnected(relay.id);
    ASSERT_TRUE(send(0, true));
    EXPECT_EQ(live(sender, viewer), 1u);
    EXPECT_EQ(sender.session.outgoing_stats(2)->subscribers, 1u);
}

TEST_F(RelayTest, AViewerThatStopsWatchingIsPassedNothingMore) {
    ASSERT_TRUE(sender.session.relay_through(2, viewer.id, relay.id));
    hub.deliver_all();
    viewer.session.unsubscribe(sender.id, 2);
    hub.deliver_all();
    EXPECT_EQ(hub.delivered[wire::MessageKind::Forward], 2) << "to nobody now";
    EXPECT_EQ(relay.session.incoming_stats(sender.id, 2)->relaying_to, 0u);
    ASSERT_TRUE(send(0, true));
    hub.deliver_all();
    EXPECT_TRUE(relay.endpoint.lifetimes.empty());
}

TEST_F(RelayTest, ChainsOfRelaysPassItOnAtEachHop) {
    Party last(hub, clock, 4);
    for (Party* p : {&sender, &relay, &viewer}) connect(*p, last);
    hub.deliver_all();
    watch(last);
    ASSERT_TRUE(sender.session.relay_through(2, viewer.id, relay.id));
    ASSERT_TRUE(sender.session.relay_through(2, last.id, viewer.id));
    hub.deliver_all();
    for (FrameNumber n = 0; n < 3; ++n) ASSERT_TRUE(send(n, n == 0));
    hub.deliver_all();
    EXPECT_EQ(last.frames, (Frames{0, 1, 2}));
    EXPECT_EQ(live(sender, last), 0u);
}

TEST_F(RelayTest, RefusesARelayItCannotUse) {
    Party stranger(hub, clock, 4);
    connect(sender, stranger);
    hub.deliver_all();
    EXPECT_FALSE(sender.session.relay_through(9, viewer.id, relay.id)) << "no such stream";
    EXPECT_FALSE(sender.session.relay_through(2, viewer.id, stranger.id)) << "does not watch";
    EXPECT_FALSE(sender.session.relay_through(2, stranger.id, relay.id)) << "does not watch";
    EXPECT_FALSE(sender.session.relay_through(2, viewer.id, viewer.id));
    ASSERT_TRUE(sender.session.relay_through(2, viewer.id, relay.id));
    EXPECT_FALSE(sender.session.relay_through(2, relay.id, viewer.id)) << "a loop";
    EXPECT_TRUE(sender.session.relay_through(2, viewer.id, std::nullopt));
    EXPECT_TRUE(sender.session.relay_through(2, relay.id, viewer.id));
}

TEST_F(RelayTest, WhatARelayPassesOnIsSaidAgainWhenALinkLostARace) {
    ASSERT_TRUE(sender.session.relay_through(2, viewer.id, relay.id));
    hub.deliver_all();
    sender.session.on_peer_rerouted(relay.id);
    hub.deliver_all();
    EXPECT_EQ(hub.delivered[wire::MessageKind::Forward], 2);
    EXPECT_EQ(relay.session.incoming_stats(sender.id, 2)->relaying_to, 1u);
}

// ── The tree ────────────────────────────────────────────────────────────────

namespace {

/// A sender and three viewers of its screen and its sound, all connected to
/// each other as a room is.
class TreeTest : public ::testing::Test {
protected:
    TreeTest() {
        for (size_t i = 0; i < parties.size(); ++i) {
            for (size_t j = i + 1; j < parties.size(); ++j) connect(*parties[i], *parties[j]);
        }
        EXPECT_TRUE(sender.session.add_stream(screen(2), kBitrate));
        EXPECT_TRUE(sender.session.add_stream(mic()));
        hub.deliver_all();
        for (Party* viewer : {&a, &b, &c}) {
            for (StreamId stream : {2, 3})
                EXPECT_TRUE(viewer->session.subscribe(sender.id, stream));
            hub.deliver_all();
        }
    }

    static void connect(Party& x, Party& y) {
        x.session.on_peer_connected(y.id);
        y.session.on_peer_connected(x.id);
    }

    /// The paths to the three carry `bits` each for a second, every one of
    /// them having its say, then lose frames at once: the sender's uplink is
    /// full at three times that.
    void fill_uplink(uint64_t bits = 2'000'000) {
        for (int sample = 0; sample <= 11; ++sample) {
            bytes_ += bits / 80;
            for (Party* to : {&a, &b, &c}) {
                PathStats s = carried(bytes_, sample == 11 ? 1 : 0);
                if (const auto it = rtts.find(to); it != rtts.end()) s.srtt_us = it->second;
                sender.session.on_peer_path(to->id, s);
            }
            if (sample < 11) clock.advance(100ms);
        }
        ASSERT_EQ(sender.session.uplink_capacity(), 3 * bits);
        hub.deliver_all();
    }

    uint64_t bytes_ = 0;
    /// The sender's round trip to those not 50 ms from it, µs.
    std::unordered_map<const Party*, uint32_t> rtts;

    size_t relaying_to(const Party& relay, StreamId stream = 2) const {
        return relay.session.incoming_stats(sender.id, stream)->relaying_to;
    }

    ManualClock clock{Timestamp{100s}};
    Hub hub;
    Party sender{hub, clock, 1};
    Party a{hub, clock, 2};
    Party b{hub, clock, 3};
    Party c{hub, clock, 4};
    std::array<Party*, 4> parties{&sender, &a, &b, &c};
};

}  // namespace

TEST_F(TreeTest, AShortUplinkHandsViewersToRelaysUntilItCarriesOneCopy) {
    fill_uplink();
    // The last to come first, to the first: the fewest hops, then the fewest
    // passed on to.
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 2u);
    EXPECT_EQ(relaying_to(a), 2u);
    EXPECT_EQ(relaying_to(b), 0u);
    EXPECT_EQ(relaying_to(c), 0u);
    // Its peers hear of its uplink, to choose by.
    EXPECT_EQ(hub.delivered[wire::MessageKind::Uplink], 3);
}

TEST_F(TreeTest, TheOneCopyLeftClimbsPastItsPartOfTheUplink) {
    fill_uplink();
    ASSERT_EQ(sender.session.outgoing_stats(2)->relayed, 2u);
    // Found full while the copies were starting, the uplink is known narrower
    // than it is; the one path left from here follows its own rate control.
    const auto part = static_cast<uint32_t>(3 * 2'000'000 * SharedUplink::kCopies);
    for (int sample = 0; sample < 400; ++sample) {
        clock.advance(100ms);
        bytes_ += 100'000;
        sender.session.on_peer_path(a.id, carried(bytes_));
    }
    EXPECT_TRUE(sender.session.uplink_capacity());
    EXPECT_GT(sender.session.outgoing_stats(2)->bitrate, part);
}

TEST_F(TreeTest, ARelayTooFarForFramesToArriveInTimeIsPassedOver) {
    // Through a, 120 ms from here, a frame would take 60 ms and at least
    // 1.5 × 70 for a repair: more than three quarters of the 150 ms it lives.
    rtts[&a] = 120'000;
    fill_uplink();
    EXPECT_EQ(relaying_to(a), 0u);
    EXPECT_EQ(relaying_to(b), 1u) << "c; a, through b or c, would be as late";
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 1u);
}

TEST_F(TreeTest, AViewerItsRelayIsTooFarFromGoesAnotherWay) {
    fill_uplink();
    ASSERT_EQ(relaying_to(a), 2u);
    // a is 100 ms from c: 25 ms to a, then 150 for a repair, is too late.
    PathStats far = path();
    far.srtt_us = 100'000;
    a.session.on_peer_path(c.id, far);
    hub.deliver_all();
    EXPECT_EQ(relaying_to(a), 1u);
    EXPECT_EQ(relaying_to(b), 1u) << "c, through b";
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 2u);
}

TEST_F(TreeTest, AQueueOnTheWayDoesNotMakeARelayTooFar) {
    fill_uplink();
    ASSERT_EQ(relaying_to(a), 2u);
    // A queue in the sender's uplink, 200 ms of it: rate control's to empty,
    // the path to a no longer for it.
    clock.advance(100ms);
    bytes_ += 25'000;
    PathStats queued = carried(bytes_);
    queued.srtt_us = 250'000;
    sender.session.on_peer_path(a.id, queued);
    a.session.on_peer_path(c.id, path());
    hub.deliver_all();
    EXPECT_EQ(relaying_to(a), 2u);
}

TEST_F(TreeTest, SoundStaysDirect) {
    fill_uplink();
    EXPECT_EQ(sender.session.outgoing_stats(3)->relayed, 0u);
    EXPECT_EQ(relaying_to(a, 3), 0u);
}

TEST_F(TreeTest, AnUplinkWithRoomForEveryCopyHandsNobodyOn) {
    fill_uplink(20'000'000);  // 17 Mbit/s for each copy, of the 8 its stream may take
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 0u);
}

TEST_F(TreeTest, ANewViewerThroughAShortUplinkGoesStraightToARelay) {
    fill_uplink();
    Party d(hub, clock, 5);
    for (Party* p : parties) connect(*p, d);
    hub.deliver_all();
    ASSERT_TRUE(d.session.subscribe(sender.id, 2));
    hub.deliver_all();
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 3u);
    EXPECT_EQ(relaying_to(a), 3u);
    const size_t messages = sender.session.outgoing_stats(2)->messages;
    FrameInfo key;
    key.keyframe = true;
    key.capture_time = clock.now();
    ASSERT_TRUE(sender.session.send_frame(2, key, payload_for(0)));
    EXPECT_EQ(sender.session.outgoing_stats(2)->messages, messages + 1) << "to the relay alone";
}

TEST_F(TreeTest, ARelayWhoseUplinkFillsHandsAViewerOn) {
    fill_uplink();
    // The relay's own uplink was found full: of the two it passes the screen
    // on to, the last goes to another viewer, the sender's having no room.
    sender.session.on_message(a.id, wire::encode_control(wire::Uplink{3'000'000}).bytes());
    hub.deliver_all();
    EXPECT_EQ(relaying_to(a), 1u);
    EXPECT_EQ(relaying_to(b), 1u);
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 2u);

    // Its uplink no longer known to be full, it is a relay as any other again.
    sender.session.on_message(a.id, wire::encode_control(wire::Uplink{}).bytes());
    Party d(hub, clock, 5);
    for (Party* p : parties) connect(*p, d);
    hub.deliver_all();
    ASSERT_TRUE(d.session.subscribe(sender.id, 2));
    hub.deliver_all();
    EXPECT_EQ(relaying_to(a), 2u);
}

TEST_F(TreeTest, AFullRelaysViewerComesBackWhereTheSendersUplinkIsNotShort) {
    fill_uplink();
    // A minute on, the sender's uplink is not known to be short any more.
    clock.advance(SharedUplink::kMemory + 1s);
    sender.session.on_peer_path(a.id, carried(bytes_));
    ASSERT_FALSE(sender.session.uplink_capacity());

    const int asked = sender.keyframes_needed;
    sender.session.on_message(a.id, wire::encode_control(wire::Uplink{3'000'000}).bytes());
    hub.deliver_all();
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 1u);
    EXPECT_EQ(relaying_to(a), 1u);
    EXPECT_EQ(sender.keyframes_needed, asked + 1) << "sent to from here again";
}

TEST_F(TreeTest, ARelayThatLeavesHasItsViewersHandedOn) {
    fill_uplink();
    ASSERT_EQ(relaying_to(a), 2u);
    a.session.unsubscribe(sender.id, 2);
    hub.deliver_all();
    // The two left go one through the other, not both through the uplink.
    EXPECT_EQ(sender.session.outgoing_stats(2)->subscribers, 2u);
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 1u);
    EXPECT_EQ(relaying_to(b), 1u);

    // And one that goes away likewise: the last is sent to from here.
    sender.session.on_peer_disconnected(b.id);
    c.session.on_peer_disconnected(b.id);
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 0u);
}

TEST_F(TreeTest, TheUplinkForgottenIsToldToo) {
    fill_uplink();
    clock.advance(SharedUplink::kMemory + 1s);
    sender.session.on_peer_path(a.id, carried(bytes_));
    EXPECT_FALSE(sender.session.uplink_capacity());
    hub.deliver_all();
    EXPECT_EQ(hub.delivered[wire::MessageKind::Uplink], 6);
}

TEST_F(TreeTest, AViewerItsRelayIsNotConnectedToGoesThroughAnother) {
    // c is not connected to a, which the sender would hand it to first.
    a.session.on_peer_disconnected(c.id);
    c.session.on_peer_disconnected(a.id);
    fill_uplink();
    // Said to each Forward naming it that was on its way: the later ones are
    // no news to the sender.
    EXPECT_GE(hub.delivered[wire::MessageKind::CannotReach], 1);
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 2u);
    EXPECT_EQ(relaying_to(a), 1u);
    EXPECT_EQ(relaying_to(b), 1u) << "c, through b";
}

TEST_F(TreeTest, ASenderWithoutRelaysSendsEveryCopy) {
    Party lone(hub, clock, 5, SessionConfig{.relays = false});
    for (Party* p : {&a, &b}) connect(lone, *p);
    ASSERT_TRUE(lone.session.add_stream(screen(2), kBitrate));
    hub.deliver_all();
    for (Party* p : {&a, &b}) ASSERT_TRUE(p->session.subscribe(lone.id, 2));
    hub.deliver_all();
    uint64_t bytes = 0;
    for (int sample = 0; sample <= 11; ++sample) {
        bytes += 25'000;
        for (Party* to : {&a, &b}) lone.session.on_peer_path(to->id, carried(bytes, sample == 11));
        if (sample < 11) clock.advance(100ms);
    }
    ASSERT_EQ(lone.session.uplink_capacity(), 4'000'000u);
    EXPECT_EQ(lone.session.outgoing_stats(2)->relayed, 0u);
}

// ── A forwarder (SFU) ───────────────────────────────────────────────────────

namespace {

/// A sender, three viewers and a server that forwards, connected to them all;
/// the viewers are strangers to each other.
class ForwarderTest : public ::testing::Test {
protected:
    ForwarderTest() {
        for (Party* p : {&server, &a, &b, &c}) connect(sender, *p);
        for (Party* p : {&a, &b, &c}) connect(server, *p);
        EXPECT_TRUE(sender.session.add_stream(screen(2), kBitrate));
        EXPECT_TRUE(sender.session.add_stream(mic()));
        hub.deliver_all();
    }

    static void connect(Party& x, Party& y) {
        x.session.on_peer_connected(y.id);
        y.session.on_peer_connected(x.id);
    }

    void watch(std::initializer_list<Party*> viewers) {
        for (Party* viewer : viewers) {
            for (StreamId stream : {2, 3})
                EXPECT_TRUE(viewer->session.subscribe(sender.id, stream));
            hub.deliver_all();
        }
    }

    /// The paths to the three carry 2 Mbit/s each for a second, then lose
    /// frames at once: the sender's uplink is full at 6.
    void fill_uplink() {
        for (int sample = 0; sample <= 11; ++sample) {
            bytes_ += 25'000;
            for (Party* to : {&a, &b, &c})
                sender.session.on_peer_path(to->id, carried(bytes_, sample == 11 ? 1 : 0));
            if (sample < 11) clock.advance(100ms);
        }
        ASSERT_EQ(sender.session.uplink_capacity(), 6'000'000u);
        hub.deliver_all();
    }

    Result<void> send(FrameNumber number, bool keyframe) {
        FrameInfo info;
        info.number = number;
        info.keyframe = keyframe;
        info.capture_time = clock.now();
        if (!keyframe && number > 0) info.references = {number - 1};
        return sender.session.send_frame(2, info, payload_for(number));
    }

    size_t relaying_to(const Party& relay) const {
        const auto stats = relay.session.incoming_stats(sender.id, 2);
        return stats ? stats->relaying_to : 0;
    }
    bool server_watches() const {
        const auto stats = server.session.incoming_stats(sender.id, 2);
        return stats && stats->subscribed;
    }

    uint64_t bytes_ = 0;
    ManualClock clock{Timestamp{100s}};
    Hub hub;
    Party sender{hub, clock, 1};
    Party server{hub, clock, 9, SessionConfig{.forwarder = true}};
    Party a{hub, clock, 2};
    Party b{hub, clock, 3};
    Party c{hub, clock, 4};
};

}  // namespace

TEST_F(ForwarderTest, AShortUplinkHandsEveryViewerToTheForwarder) {
    watch({&a, &b, &c});
    fill_uplink();
    const OutgoingStreamStats stats = *sender.session.outgoing_stats(2);
    EXPECT_EQ(stats.subscribers, 3u) << "the forwarder is no viewer";
    EXPECT_EQ(stats.relayed, 3u);
    EXPECT_EQ(relaying_to(server), 3u);
    EXPECT_TRUE(server_watches());
    // Sound stays direct.
    EXPECT_EQ(sender.session.outgoing_stats(3)->relayed, 0u);
}

TEST_F(ForwarderTest, ItPassesFramesOnFromAKeyframeAndKeepsNone) {
    watch({&a, &b, &c});
    fill_uplink();
    const size_t messages = sender.session.outgoing_stats(2)->messages;
    for (FrameNumber n = 0; n < 4; ++n) {
        ASSERT_TRUE(send(n, n == 0));
        hub.deliver_all();
    }
    EXPECT_EQ(sender.session.outgoing_stats(2)->messages, messages + 4) << "one copy a frame";
    for (Party* viewer : {&a, &b, &c}) EXPECT_EQ(viewer->frames, (Frames{0, 1, 2, 3}));
    EXPECT_TRUE(server.frames.empty());
    EXPECT_EQ(server.session.incoming_stats(sender.id, 2)->relayed_messages, 12u);
    // Sealed end to end: the viewers have the key, the forwarder not.
    EXPECT_EQ(hub.delivered[wire::MessageKind::FrameKey], 3);
    for (Party* viewer : {&a, &b, &c})
        EXPECT_EQ(viewer->session.incoming_stats(sender.id, 2)->opened, 4u);
    // Nor does it ask for keyframes of its own as time passes.
    const size_t requests = sender.session.outgoing_stats(2)->keyframe_requests;
    for (int i = 0; i < 50; ++i) {
        clock.advance(20ms);
        server.session.tick();
        hub.deliver_all();
    }
    EXPECT_EQ(sender.session.outgoing_stats(2)->keyframe_requests, requests);
}

TEST_F(ForwarderTest, ViewersTheServerCarriesAnywayGoThroughItsForwarder) {
    sender.endpoint.carried = {a.id, b.id};
    watch({&a, &b, &c});
    EXPECT_FALSE(sender.session.uplink_capacity());
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 2u);
    EXPECT_EQ(relaying_to(server), 2u) << "a and b, not c";
}

TEST_F(ForwarderTest, AForwarderLeftWithNobodyIsLetGo) {
    watch({&a, &b, &c});
    fill_uplink();
    ASSERT_TRUE(server_watches());
    for (Party* viewer : {&a, &b, &c}) {
        viewer->session.unsubscribe(sender.id, 2);
        hub.deliver_all();
    }
    EXPECT_EQ(sender.session.outgoing_stats(2)->subscribers, 0u);
    EXPECT_FALSE(server_watches());
    const size_t messages = sender.session.outgoing_stats(2)->messages;
    ASSERT_TRUE(send(0, true));
    EXPECT_EQ(sender.session.outgoing_stats(2)->messages, messages) << "sent to nobody";
}

TEST_F(ForwarderTest, AViewerTheForwarderCannotReachGoesAnotherWay) {
    server.session.on_peer_disconnected(c.id);
    c.session.on_peer_disconnected(server.id);
    watch({&a, &b, &c});
    fill_uplink();
    EXPECT_GE(hub.delivered[wire::MessageKind::CannotReach], 1);
    EXPECT_EQ(relaying_to(server), 2u) << "a and b";
    // c through a or b, which are not connected to it either: back from the sender.
    EXPECT_EQ(sender.session.outgoing_stats(2)->relayed, 2u);
}

TEST_F(ForwarderTest, AServerThatLeavesHandsItsViewersBack) {
    watch({&a, &b, &c});
    fill_uplink();
    ASSERT_EQ(relaying_to(server), 3u);
    sender.session.on_peer_disconnected(server.id);
    for (Party* viewer : {&a, &b, &c}) viewer->session.on_peer_disconnected(server.id);
    hub.deliver_all();
    // From the sender again — the viewers, strangers to each other, cannot
    // pass it on — from a keyframe.
    EXPECT_EQ(sender.session.outgoing_stats(2)->subscribers, 3u);
    for (FrameNumber n = 0; n < 2; ++n) {
        ASSERT_TRUE(send(n, n == 0));
        hub.deliver_all();
    }
    for (Party* viewer : {&a, &b, &c}) EXPECT_EQ(viewer->frames, (Frames{0, 1}));
}

// ── Sealed end to end ───────────────────────────────────────────────────────

namespace {

/// A sender, a viewer that may relay and one more, each connected to each.
class SealingTest : public ::testing::Test {
protected:
    explicit SealingTest(SessionConfig sending = {}) : sender(hub, clock, 1, std::move(sending)) {
        for (auto [x, y] : {std::pair{&sender, &relay}, {&sender, &viewer}, {&relay, &viewer}}) {
            x->session.on_peer_connected(y->id);
            y->session.on_peer_connected(x->id);
        }
        EXPECT_TRUE(sender.session.add_stream(screen(2), kBitrate));
        hub.deliver_all();
    }

    void watch(Party& who) {
        ASSERT_TRUE(who.session.subscribe(sender.id, 2));
        hub.deliver_all();
    }

    /// Frame `number` of the screen, sent; its message to `to`, as it went.
    std::vector<uint8_t> send(FrameNumber number, const Party& to) {
        FrameInfo info;
        info.number = number;
        info.keyframe = number == 0;
        if (number > 0) info.references = {number - 1};
        info.capture_time = clock.now();
        EXPECT_TRUE(sender.session.send_frame(2, info, payload_for(number)));
        for (const Message& m : hub.queue) {
            if (m.live && m.from == sender.id && m.to == to.id)
                return {m.bytes.bytes().begin(), m.bytes.bytes().end()};
        }
        return {};
    }

    uint64_t unopened(const Party& who) const {
        return who.session.incoming_stats(sender.id, 2)->unopened;
    }

    ManualClock clock{Timestamp{100s}};
    Hub hub;
    Party sender;
    Party relay{hub, clock, 2};
    Party viewer{hub, clock, 3};
};

std::vector<uint8_t> bytes_of(const Buffer& buffer) {
    return {buffer.bytes().begin(), buffer.bytes().end()};
}

}  // namespace

TEST_F(SealingTest, AViewerOpensWhatItsSenderSeals) {
    watch(viewer);
    EXPECT_EQ(hub.delivered[wire::MessageKind::FrameKey], 1) << "the key, as it subscribed";
    const std::vector<uint8_t> sent = send(0, viewer);
    const Result<wire::FrameHeader> header = wire::read_frame_header(sent);
    ASSERT_TRUE(header);
    EXPECT_TRUE(header->sealed);
    const std::vector<uint8_t> plain = bytes_of(payload_for(0).freeze());
    EXPECT_EQ(std::ranges::search(sent, plain).begin(), sent.end()) << "the picture, in the clear";
    hub.deliver_all();
    ASSERT_EQ(viewer.frames, Frames{0});
    EXPECT_EQ(bytes_of(viewer.payloads.front()), plain);
    EXPECT_EQ(viewer.session.incoming_stats(sender.id, 2)->opened, 1u);
}

TEST_F(SealingTest, AFramePassedOnUnsealedIsDropped) {
    watch(viewer);
    FrameInfo info;
    info.stream = 2;
    info.keyframe = true;
    info.capture_time = clock.now();
    const Buffer plain = *wire::encode_frame(info, payload_for(0));
    // Passed on by another: anyone's word.
    viewer.session.on_message(relay.id,
                              wire::encode_relayed_frame(sender.id, plain.bytes()).bytes());
    EXPECT_TRUE(viewer.frames.empty());
    EXPECT_EQ(unopened(viewer), 1u);
    // From its sender itself, over the connection only it holds: its own.
    viewer.session.on_message(sender.id, plain.bytes());
    EXPECT_EQ(viewer.frames, Frames{0});
}

TEST_F(SealingTest, AFrameAlteredOnTheWayIsDropped) {
    watch(viewer);
    std::vector<uint8_t> payload_altered = send(0, viewer);
    hub.queue.clear();
    payload_altered.back() ^= 1;
    std::vector<uint8_t> header_altered = send(1, viewer);
    hub.queue.clear();
    header_altered[1] |= 1;  // made a keyframe
    for (const auto& altered : {payload_altered, header_altered}) {
        viewer.session.on_message(relay.id, wire::encode_relayed_frame(sender.id, altered).bytes());
    }
    EXPECT_TRUE(viewer.frames.empty());
    EXPECT_EQ(unopened(viewer), 2u);
}

TEST_F(SealingTest, AViewerThatGoesCannotOpenWhatIsSentASecondLater) {
    watch(relay);
    watch(viewer);
    sender.session.on_peer_disconnected(relay.id);
    hub.deliver_all();
    EXPECT_EQ(hub.delivered[wire::MessageKind::FrameKey], 3) << "a new one, to the one left";

    // Sealed under the old key a moment longer: the one left has the new one
    // on its way.
    const std::vector<uint8_t> soon = send(0, viewer);
    hub.deliver_all();
    relay.session.on_message(sender.id, soon);
    EXPECT_EQ(relay.frames, Frames{0});

    clock.advance(1s);
    const std::vector<uint8_t> later = send(1, viewer);
    hub.deliver_all();
    EXPECT_EQ(viewer.frames, (Frames{0, 1}));
    relay.session.on_message(sender.id, later);
    EXPECT_EQ(relay.frames, Frames{0});
    EXPECT_EQ(unopened(relay), 1u);
}

namespace {

/// A sender that keeps its key from the viewer.
class SealingWithholdingTest : public SealingTest {
protected:
    SealingWithholdingTest()
        : SealingTest(
              SessionConfig{.entitled = [this](const PeerId& peer) { return peer != viewer.id; }}) {
    }
};

/// A sender that seals nothing.
class UnsealedTest : public SealingTest {
protected:
    UnsealedTest() : SealingTest(SessionConfig{.seal = false}) {}
};

}  // namespace

TEST_F(SealingWithholdingTest, WhoMayReadIsTheApplicationsToSay) {
    watch(relay);
    watch(viewer);
    EXPECT_EQ(hub.delivered[wire::MessageKind::FrameKey], 1);
    send(0, viewer);
    hub.deliver_all();
    EXPECT_EQ(relay.frames, Frames{0});
    EXPECT_TRUE(viewer.frames.empty());
    EXPECT_EQ(unopened(viewer), 1u);
}

TEST_F(UnsealedTest, ASenderThatDoesNotSealSendsFramesAsTheyAre) {
    watch(viewer);
    EXPECT_EQ(hub.delivered[wire::MessageKind::FrameKey], 0);
    EXPECT_FALSE(wire::read_frame_header(send(0, viewer))->sealed);
    hub.deliver_all();
    EXPECT_EQ(viewer.frames, Frames{0});
}
