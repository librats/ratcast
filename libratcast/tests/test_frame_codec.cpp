#include "wire/frame_codec.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include "wire/message.h"

using namespace ratcast;
using namespace ratcast::wire;
using namespace std::chrono_literals;

namespace {

std::vector<uint8_t> payload_bytes(size_t size) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(i * 31 + 7);
    return bytes;
}

MutableBuffer payload_of(std::span<const uint8_t> bytes) {
    MutableBuffer payload = allocate_frame_payload(bytes.size());
    payload.append(bytes);
    return payload;
}

Buffer encode_ok(const FrameInfo& info, std::span<const uint8_t> payload) {
    Result<Buffer> message = encode_frame(info, payload_of(payload));
    EXPECT_TRUE(message);
    return message ? *message : Buffer{};
}

FrameInfo delta_frame() {
    FrameInfo info;
    info.stream = 3;
    info.number = 1000;
    info.capture_time = Timestamp{123456789us};
    info.references = {999};
    return info;
}

FrameInfo layered_frame() {
    FrameInfo info;
    info.stream = 70000;
    info.number = 5'000'000'000;
    info.capture_time = Timestamp{86400s};
    info.keyframe = true;
    info.temporal_layer = 2;
    info.spatial_layer = 1;
    info.references = {4'999'999'999, 4'999'999'990, 4'999'000'000, 0};
    return info;
}

std::vector<uint8_t> bytes_of(const Buffer& buffer) {
    return {buffer.data(), buffer.data() + buffer.size()};
}

Errc decode_error(std::vector<uint8_t> bytes) {
    const Result<EncodedFrame> frame = decode_frame(Buffer::copy_of(bytes));
    EXPECT_FALSE(frame);
    return frame ? Errc{} : frame.error();
}

}  // namespace

// ── Round trips ─────────────────────────────────────────────────────────────

TEST(FrameCodec, RoundTripsAFrame) {
    for (const FrameInfo& info : {delta_frame(), layered_frame(), FrameInfo{}}) {
        const auto payload = payload_bytes(777);
        const Buffer message = encode_ok(info, payload);
        EXPECT_EQ(message.size(), frame_header_size(info) + payload.size());

        const Result<EncodedFrame> frame = decode_frame(message);
        ASSERT_TRUE(frame) << to_string(frame.error());
        EXPECT_EQ(frame->info, info);
        EXPECT_TRUE(std::ranges::equal(frame->payload.bytes(), payload));
    }
}

TEST(FrameCodec, HeaderOfATypicalDeltaFrameIsSmall) {
    EXPECT_LE(frame_header_size(delta_frame()), 12u);
}

TEST(FrameCodec, CarriesAnEmptyPayload) {
    const Buffer message = encode_ok(delta_frame(), {});
    const Result<EncodedFrame> frame = decode_frame(message);
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->payload.empty());
}

TEST(FrameCodec, WritesTheHeaderIntoTheHeadroomWithoutCopying) {
    const auto bytes = payload_bytes(4096);
    MutableBuffer payload = payload_of(bytes);
    const uint8_t* payload_at = payload.data();

    const Result<Buffer> message = encode_frame(delta_frame(), std::move(payload));
    ASSERT_TRUE(message);
    EXPECT_EQ(message->data() + frame_header_size(delta_frame()), payload_at);
}

TEST(FrameCodec, CopiesAPayloadWithoutHeadroomOnce) {
    const auto bytes = payload_bytes(100);
    MutableBuffer bare = MutableBuffer::allocate(bytes.size());
    bare.append(bytes);

    const Result<Buffer> message = encode_frame(delta_frame(), std::move(bare));
    ASSERT_TRUE(message);
    const Result<EncodedFrame> frame = decode_frame(*message);
    ASSERT_TRUE(frame);
    EXPECT_TRUE(std::ranges::equal(frame->payload.bytes(), bytes));
}

TEST(FrameCodec, DecodedPayloadIsASliceOfTheMessage) {
    const Buffer message = encode_ok(delta_frame(), payload_bytes(64));
    const Result<EncodedFrame> frame = decode_frame(message);
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->payload.data(), message.data() + frame_header_size(delta_frame()));
    EXPECT_EQ(message.use_count(), 2u);
}

TEST(FrameCodec, MessageKindIsReadableUpFront) {
    const Buffer message = encode_ok(delta_frame(), payload_bytes(8));
    const Result<MessageKind> kind = peek_kind(message.bytes());
    ASSERT_TRUE(kind);
    EXPECT_EQ(*kind, MessageKind::Frame);
}

// ── What cannot be encoded ──────────────────────────────────────────────────

TEST(FrameCodec, RefusesFramesItCannotEncode) {
    const auto refuse = [](FrameInfo info) {
        EXPECT_FALSE(is_encodable(info));
        const Result<Buffer> message = encode_frame(info, payload_of(payload_bytes(4)));
        ASSERT_FALSE(message);
        EXPECT_EQ(message.error(), Errc::InvalidArgument);
    };

    FrameInfo self_reference = delta_frame();
    self_reference.references = {self_reference.number};
    refuse(self_reference);

    FrameInfo future_reference = delta_frame();
    future_reference.references = {future_reference.number + 1};
    refuse(future_reference);

    FrameInfo temporal = delta_frame();
    temporal.temporal_layer = FrameInfo::kMaxTemporalLayer + 1;
    refuse(temporal);

    FrameInfo spatial = delta_frame();
    spatial.spatial_layer = FrameInfo::kMaxSpatialLayer + 1;
    refuse(spatial);

    FrameInfo before_epoch = delta_frame();
    before_epoch.capture_time = Timestamp{-1us};
    refuse(before_epoch);
}

// ── What is refused on decode ───────────────────────────────────────────────

TEST(FrameCodec, EveryCutInsideTheHeaderIsTruncated) {
    const FrameInfo info = layered_frame();
    const auto message = bytes_of(encode_ok(info, payload_bytes(32)));
    for (size_t cut = 0; cut < frame_header_size(info); ++cut) {
        EXPECT_EQ(decode_error({message.begin(), message.begin() + static_cast<long>(cut)}),
                  Errc::Truncated)
            << "cut at " << cut;
    }
}

TEST(FrameCodec, RefusesAnotherWireVersion) {
    auto message = bytes_of(encode_ok(delta_frame(), payload_bytes(8)));
    message[0] = static_cast<uint8_t>((kWireVersion + 1) << 5 | (message[0] & 0x1F));
    EXPECT_EQ(decode_error(message), Errc::UnsupportedVersion);
}

TEST(FrameCodec, RefusesAnUnknownKind) {
    auto message = bytes_of(encode_ok(delta_frame(), payload_bytes(8)));
    message[0] = static_cast<uint8_t>(kWireVersion << 5 | 0x1F);
    EXPECT_EQ(decode_error(message), Errc::UnknownKind);
}

TEST(FrameCodec, RefusesReservedFlags) {
    auto message = bytes_of(encode_ok(delta_frame(), payload_bytes(8)));
    message[1] |= 1 << 3;
    EXPECT_EQ(decode_error(message), Errc::Malformed);
}

TEST(FrameCodec, RefusesMoreReferencesThanItHolds) {
    // flags: 9 references; then stream 0, frame 100, time 0 and nine deltas of 1.
    std::vector<uint8_t> message = {message_prefix(MessageKind::Frame), 9 << 4, 0, 100, 0};
    message.insert(message.end(), 9, 1);
    EXPECT_EQ(decode_error(message), Errc::Malformed);
}

TEST(FrameCodec, RefusesReferencesOutsideTheStream) {
    const uint8_t prefix = message_prefix(MessageKind::Frame);
    // frame 5 referencing "frame 5": a delta of 0
    EXPECT_EQ(decode_error({prefix, 1 << 4, 0, 5, 0, 0}), Errc::Malformed);
    // frame 5 referencing frame -1: a delta of 6
    EXPECT_EQ(decode_error({prefix, 1 << 4, 0, 5, 0, 6}), Errc::Malformed);
}

TEST(FrameCodec, RefusesAnAllZeroOrReservedLayersByte) {
    const uint8_t prefix = message_prefix(MessageKind::Frame);
    const uint8_t layers_flag = 1 << 1;
    EXPECT_EQ(decode_error({prefix, layers_flag, 0, 5, 0, 0x00}), Errc::Malformed);
    EXPECT_EQ(decode_error({prefix, layers_flag, 0, 5, 0, 0x20}), Errc::Malformed);
}

TEST(FrameCodec, RefusesAStreamIdPastThirtyTwoBits) {
    std::vector<uint8_t> message = {message_prefix(MessageKind::Frame), 0};
    const auto stream = std::array<uint8_t, 5>{0x80, 0x80, 0x80, 0x80, 0x10};  // 2^32
    message.insert(message.end(), stream.begin(), stream.end());
    message.insert(message.end(), {5, 0});
    EXPECT_EQ(decode_error(message), Errc::Malformed);
}

TEST(FrameCodec, RefusesACaptureTimePastTheClockRange) {
    std::vector<uint8_t> message = {message_prefix(MessageKind::Frame), 0, 0, 5};
    std::array<uint8_t, 10> time;  // 2^63
    time.fill(0x80);
    time[9] = 0x01;
    message.insert(message.end(), time.begin(), time.end());
    EXPECT_EQ(decode_error(message), Errc::Malformed);
}

TEST(FrameCodec, SkipsAnExtensionBlock) {
    const uint8_t prefix = message_prefix(MessageKind::Frame);
    const uint8_t flags = 1 << 4 | 1 << 2;  // one reference, extensions
    // stream 2, frame 9, time 1, delta 1, a 3-byte extension, then a 2-byte payload
    const std::vector<uint8_t> message = {prefix, flags, 2,    9,    1,    1,
                                          3,      0xE1,  0xE2, 0xE3, 0xAA, 0xBB};

    const Result<EncodedFrame> frame = decode_frame(Buffer::copy_of(message));
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->info.number, 9u);
    EXPECT_EQ(frame->info.references, (FrameReferences{8}));
    EXPECT_EQ(bytes_of(frame->payload), (std::vector<uint8_t>{0xAA, 0xBB}));

    // An extension block longer than what is left is truncated.
    EXPECT_EQ(decode_error({prefix, flags, 2, 9, 1, 1, 9, 0xE1}), Errc::Truncated);
}

// ── Arbitrary input ─────────────────────────────────────────────────────────

// Whatever arrives, decoding either fails cleanly or yields a frame that
// encodes back to the very bytes it came from (one encoding per frame).
TEST(FrameCodec, ArbitraryBytesDecodeToCanonicalFramesOrFail) {
    std::mt19937_64 rng(20261016);
    const FrameInfo seeds[] = {delta_frame(), layered_frame(), FrameInfo{}};
    size_t decoded = 0;

    for (int round = 0; round < 200'000; ++round) {
        std::vector<uint8_t> bytes;
        if (round % 2 == 0) {
            // A valid message with a few bytes flipped: mostly near misses.
            bytes = bytes_of(encode_ok(seeds[round % 3], payload_bytes(rng() % 16)));
            const size_t flips = 1 + rng() % 3;
            for (size_t i = 0; i < flips; ++i)
                bytes[rng() % bytes.size()] ^= uint8_t(1u << rng() % 8);
        } else {
            bytes.resize(rng() % 40);
            for (uint8_t& byte : bytes) byte = static_cast<uint8_t>(rng());
            if (!bytes.empty() && rng() % 2) bytes[0] = message_prefix(MessageKind::Frame);
        }

        const Result<EncodedFrame> frame = decode_frame(Buffer::copy_of(bytes));
        if (!frame) continue;
        ++decoded;

        const bool had_extensions = (bytes[1] & (1 << 2)) != 0;
        if (had_extensions) continue;  // dropped on re-encoding, by design
        const Buffer again = encode_ok(frame->info, frame->payload.bytes());
        EXPECT_EQ(bytes_of(again), bytes);
    }
    EXPECT_GT(decoded, 1000u);  // the near misses must exercise the success path too
}
