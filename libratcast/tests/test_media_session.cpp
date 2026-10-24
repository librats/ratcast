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

    /// Deliver everything queued, including what deliveries queue in turn.
    void deliver_all() {
        while (!queue.empty()) {
            Message m = std::move(queue.front());
            queue.pop_front();
            if (m.live && hold && hold(m)) {
                held.push_back(std::move(m));
                continue;
            }
            const bool lost = m.live && lose && lose(m);
            if (!lost) sessions.at(m.to)->on_message(m.from, m.bytes.bytes());
            if (m.live && m.tag != 0) sessions.at(m.from)->on_live_outcome(m.to, m.tag, !lost);
        }
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
    Endpoint(Hub& hub, PeerId self) : hub_(hub), self_(std::move(self)) {}

    void send_reliable(const PeerId& peer, const Buffer& message) override {
        hub_.queue.push_back({self_, peer, message, false});
    }
    bool send_live(const PeerId& peer, const Buffer& message, Duration lifetime, uint64_t tag,
                   LiveRank rank) override {
        EXPECT_GT(lifetime, Duration::zero());
        lifetimes.push_back(lifetime);
        tags.push_back(tag);
        ranks.push_back(rank);
        hub_.queue.push_back({self_, peer, message, true, tag});
        return !full_.contains(peer);
    }
    bool writable(const PeerId& peer) const override { return !full_.contains(peer); }
    size_t max_live_message() const override { return 64 * 1024; }

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
    PeerId self_;
    std::set<PeerId> full_;
};

/// One participant: a session, its network endpoint and what its handlers saw.
struct Party {
    Party(Hub& hub, Clock& clock, uint8_t seed)
        : id(make_peer(seed)),
          endpoint(hub, id),
          session(endpoint, clock, handlers(), SessionConfig{}) {
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
    Party sender{hub, clock, 1};
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
