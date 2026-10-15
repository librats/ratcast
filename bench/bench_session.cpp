// What MediaSession adds per frame on top of the wire format: finding the
// stream, deciding per subscriber, handing the message to the network — and on
// the receiving side, copying it in, tracking it and handing it on.
//
// The network here does nothing, so these are the session's own costs. A real
// network copies each message once more (librats' Node::send), which is not
// counted here.

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "framework/alloc_track.h"
#include "framework/bench.h"
#include "ratcast/session/media_session.h"
#include "wire/control_codec.h"
#include "wire/frame_codec.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr size_t kDeltaFrame = 16 * 1024;

class NullNetwork final : public MediaNetwork {
public:
    void send_reliable(const PeerId&, const Buffer&) override {}
    bool send_live(const PeerId&, const Buffer& message, Duration) override {
        bench::do_not_optimize(message.data());
        return true;
    }
    bool writable(const PeerId&) const override { return true; }
    size_t max_live_message() const override { return 4 * 1024 * 1024; }
};

PeerId make_peer(uint32_t seed) {
    std::array<uint8_t, 32> key{};
    std::memcpy(key.data(), &seed, sizeof seed);
    return PeerId::from_public_key(key.data(), key.size());
}

StreamDescription screen() {
    StreamDescription d;
    d.id = 1;
    d.codec = Codec::AV1;
    d.format = VideoFormat{1920, 1080, 60, 1};
    d.lifetime = 150ms;
    d.label = "screen";
    return d;
}

/// A sender with `subscribers` subscribed peers, past their first keyframe.
struct Sender {
    explicit Sender(size_t subscribers) {
        (void)session.add_stream(screen());
        const Buffer subscribe = wire::encode_control(wire::Subscribe{1});
        for (uint32_t i = 0; i < subscribers; ++i) {
            const PeerId peer = make_peer(i + 1);
            session.on_peer_connected(peer);
            session.on_message(peer, subscribe.bytes());
        }
        send(true);
    }

    void send(bool keyframe = false) {
        FrameInfo info;
        info.number = next++;
        info.keyframe = keyframe;
        if (!keyframe) info.references = {info.number - 1};
        MutableBuffer payload = allocate_frame_payload(kDeltaFrame);
        payload.append(kDeltaFrame);
        (void)session.send_frame(1, info, std::move(payload));
    }

    ManualClock clock;
    NullNetwork network;
    MediaSession session{network, clock, SessionHandlers{}};
    FrameNumber next = 0;
};

/// A receiver subscribed to one stream of one peer.
struct Receiver {
    Receiver() {
        session.on_peer_connected(peer);
        session.on_message(peer, wire::encode_control(wire::Announce{screen()}).bytes());
        (void)session.subscribe(peer, 1);
        receive(true);
    }

    void receive(bool keyframe = false) {
        FrameInfo info;
        info.stream = 1;
        info.number = next++;
        info.keyframe = keyframe;
        if (!keyframe) info.references = {info.number - 1};
        MutableBuffer payload = allocate_frame_payload(kDeltaFrame);
        payload.append(kDeltaFrame);
        const Buffer message = *wire::encode_frame(info, std::move(payload));
        session.on_message(peer, message.bytes());
    }

    ManualClock clock;
    NullNetwork network;
    uint64_t delivered = 0;
    MediaSession session{
        network, clock,
        SessionHandlers{.frame_received = [this](const PeerId&, EncodedFrame) { ++delivered; }}};
    PeerId peer = make_peer(99);
    FrameNumber next = 0;
};

}  // namespace

int main() {
    {
        bench::Bench b("MediaSession, 16 KiB delta frames");

        b.group("send_frame");
        b.items(1);
        for (size_t subscribers : {1, 10, 50}) {
            auto sender = std::make_unique<Sender>(subscribers);
            b.run(std::to_string(subscribers) + " subscriber(s)", [&] { sender->send(); });
        }

        b.group("receive: encode + on_message + hand on");
        b.items(1);
        Receiver receiver;
        b.run("1 stream", [&] { receiver.receive(); });
    }

    constexpr int kFrames = 10'000;
    track::Stats send{};
    track::Stats receive{};
    {
        Sender sender(50);
        for (int i = 0; i < 100; ++i) sender.send();  // warm the pool
        track::Scope scope(send);
        for (int i = 0; i < kFrames; ++i) sender.send();
    }
    {
        Receiver receiver;
        for (int i = 0; i < 100; ++i) receiver.receive();
        track::Scope scope(receive);
        for (int i = 0; i < kFrames; ++i) receiver.receive();
    }
    std::printf("\nHeap allocations per frame, steady state (%d frames):\n", kFrames);
    std::printf("  send_frame, 50 subscribers   %.3f\n", double(send.allocs) / kFrames);
    std::printf("  receive and hand on          %.3f\n", double(receive.allocs) / kFrames);
    return 0;
}
