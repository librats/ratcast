// MediaSession end to end over an in-memory network, in virtual time: every
// message is queued, and delivered only when the test says so — which is
// where it drops, reorders or holds them.

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
};

/// The wire between every session in a test.
class Hub {
public:
    std::deque<Message> queue;
    std::map<PeerId, MediaSession*> sessions;
    /// Live messages it returns true for are lost.
    std::function<bool(const Message&)> lose;

    /// Deliver everything queued, including what deliveries queue in turn.
    void deliver_all() {
        while (!queue.empty()) {
            Message m = std::move(queue.front());
            queue.pop_front();
            if (m.live && lose && lose(m)) continue;
            sessions.at(m.to)->on_message(m.from, m.bytes.bytes());
        }
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
    bool send_live(const PeerId& peer, const Buffer& message, Duration lifetime) override {
        EXPECT_GT(lifetime, Duration::zero());
        hub_.queue.push_back({self_, peer, message, true});
        return !full_.contains(peer);
    }
    bool writable(const PeerId& peer) const override { return !full_.contains(peer); }
    size_t max_live_message() const override { return 64 * 1024; }

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
    hub.deliver_all();
    hub.lose = nullptr;
    ASSERT_TRUE(send(2, false));
    hub.deliver_all();
    EXPECT_EQ(receiver.frames, Frames{0});
    EXPECT_EQ(sender.keyframes_needed, 1);

    advance(150ms);  // frame 1 is given up: 2 cannot be decoded
    EXPECT_EQ(sender.keyframes_needed, 2);
    EXPECT_EQ(sender.session.outgoing_stats(1)->keyframe_requests, 1u);

    ASSERT_TRUE(send(3, true));
    hub.deliver_all();
    EXPECT_EQ(receiver.frames, (Frames{0, 3}));

    const auto stats = receiver.session.incoming_stats(sender.id, 1);
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->frames.lost, 1u);
    EXPECT_EQ(stats->frames.undecodable, 1u);
}

TEST_F(MediaSessionTest, FramesExpiringOnTheWayAreAKeyframeNeeded) {
    subscribe(receiver);
    ASSERT_TRUE(send(0, true));
    hub.deliver_all();
    EXPECT_EQ(sender.keyframes_needed, 1);
    sender.session.on_peer_path(receiver.id, path());
    EXPECT_EQ(sender.keyframes_needed, 1);

    // The frame behind the keyframe expired: the receiver cannot decode what
    // follows, and the sender need not wait for it to say so.
    sender.session.on_peer_path(receiver.id, path(1));
    EXPECT_EQ(sender.keyframes_needed, 1);  // held back: a keyframe was just sent...
    advance(200ms);
    EXPECT_EQ(sender.keyframes_needed, 2);  // ...until min_keyframe_interval has passed
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

// ── Peers coming and going ──────────────────────────────────────────────────

TEST_F(MediaSessionTest, ADisconnectEndsStreamsAndSubscriptions) {
    subscribe(receiver);
    receiver.session.on_peer_disconnected(sender.id);
    EXPECT_EQ(receiver.withdrawn, std::vector<StreamId>{1});
    EXPECT_FALSE(receiver.session.incoming_stats(sender.id, 1));

    sender.session.on_peer_disconnected(receiver.id);
    EXPECT_EQ(sender.session.outgoing_stats(1)->subscribers, 0u);
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
