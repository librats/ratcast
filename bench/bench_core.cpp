// What a frame costs libratcast before it reaches the transport: getting
// storage for it, putting a header in front of it, reading the header back.
//
// Sizes are those of real encoded frames: a 1080p60 delta at ~8 Mbit/s is
// ~16 KiB, a 1080p keyframe a few hundred KiB, a 4K keyframe several MiB.

#include <cstdio>
#include <cstring>
#include <memory>

#include "framework/alloc_track.h"
#include "framework/bench.h"
#include "ratcast/buffer.h"
#include "ratcast/frame.h"
#include "wire/frame_codec.h"

using namespace ratcast;

namespace {

constexpr size_t kDeltaFrame = 16 * 1024;
constexpr size_t kKeyframe = 256 * 1024;
constexpr size_t kLargeKeyframe = 4 * 1024 * 1024;

FrameInfo delta_info(FrameNumber number) {
    FrameInfo info;
    info.stream = 1;
    info.number = number;
    info.capture_time = Timestamp{Duration{1'000'000 + static_cast<int64_t>(number) * 16'667}};
    info.references = {number - 1};
    return info;
}

// An encoder writing a frame: storage for it, then every byte of it.
void storage_benchmarks(bench::Bench& b) {
    for (size_t size : {kDeltaFrame, kKeyframe, kLargeKeyframe}) {
        b.group("allocate, fill, release " + std::to_string(size / 1024) + " KiB");
        b.bytes(static_cast<double>(size));

        b.run("new[] / delete[]", [size] {
            std::unique_ptr<uint8_t[]> bytes(new uint8_t[size]);
            std::memset(bytes.get(), 0x5A, size);
            bench::do_not_optimize(bytes[size - 1]);
        });
        b.run("MutableBuffer (pooled)", [size] {
            MutableBuffer buffer = allocate_frame_payload(size);
            std::memset(buffer.append(size), 0x5A, size);
            const Buffer frozen = std::move(buffer).freeze();
            bench::do_not_optimize(frozen.data()[size - 1]);
        });
    }
}

void header_benchmarks(bench::Bench& b) {
    b.group("frame header, 16 KiB delta frame");
    b.items(1);

    FrameNumber number = 1;
    b.run("encode (pooled payload)", [&number] {
        MutableBuffer payload = allocate_frame_payload(kDeltaFrame);
        payload.append(kDeltaFrame);
        Result<Buffer> message = wire::encode_frame(delta_info(number++), std::move(payload));
        bench::do_not_optimize(message->data());
    });

    MutableBuffer payload = allocate_frame_payload(kDeltaFrame);
    std::memset(payload.append(kDeltaFrame), 0x5A, kDeltaFrame);
    const Buffer message = *wire::encode_frame(delta_info(1000), std::move(payload));
    b.run("decode (payload sliced)", [&message] {
        Result<EncodedFrame> frame = wire::decode_frame(message);
        bench::do_not_optimize(frame->payload.data());
    });

    b.group("sharing a frame");
    b.items(1);
    b.run("copy + slice + release", [&message] {
        const Buffer copy = message;  // NOLINT(performance-unnecessary-copy-initialization)
        const Buffer payload = copy.slice(16);
        bench::do_not_optimize(payload.data());
    });
}

// The steady-state allocation count of the send and receive paths, which the
// timing table cannot show: it should be zero.
void allocation_report() {
    constexpr int kFrames = 10'000;
    track::Stats send{};
    track::Stats receive{};
    {
        // Warm the pool to its steady state first.
        for (FrameNumber n = 1; n < 8; ++n) {
            (void)wire::encode_frame(delta_info(n), allocate_frame_payload(kDeltaFrame));
        }
        track::Scope scope(send);
        for (FrameNumber n = 1; n <= kFrames; ++n) {
            MutableBuffer payload = allocate_frame_payload(kDeltaFrame);
            payload.append(kDeltaFrame);
            Result<Buffer> message = wire::encode_frame(delta_info(n), std::move(payload));
            Result<EncodedFrame> frame = wire::decode_frame(*message);
            bench::do_not_optimize(frame->payload.data());
        }
    }
    {
        MutableBuffer payload = allocate_frame_payload(kDeltaFrame);
        payload.append(kDeltaFrame);
        const Buffer message = *wire::encode_frame(delta_info(1), std::move(payload));
        track::Scope scope(receive);
        for (int i = 0; i < kFrames; ++i) {
            const Buffer copy = Buffer::copy_of(message.bytes());  // what a receiver does
            Result<EncodedFrame> frame = wire::decode_frame(copy);
            bench::do_not_optimize(frame->payload.data());
        }
    }
    std::printf("\nHeap allocations per frame, steady state (pooled, %d frames):\n", kFrames);
    std::printf("  encode + decode          %.3f\n", double(send.allocs) / kFrames);
    std::printf("  copy received + decode   %.3f\n", double(receive.allocs) / kFrames);
}

}  // namespace

int main() {
    {
        bench::Bench b("libratcast core");
        storage_benchmarks(b);
        header_benchmarks(b);
    }
    allocation_report();
    return 0;
}
