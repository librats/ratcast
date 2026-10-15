#include "wire/frame_codec.h"

#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "wire/message.h"

namespace ratcast::wire {
namespace {

constexpr uint8_t kKeyframeFlag = 1 << 0;
constexpr uint8_t kLayersFlag = 1 << 1;
constexpr uint8_t kExtensionsFlag = 1 << 2;
constexpr uint8_t kReservedFlags = 1 << 3;
constexpr unsigned kReferenceCountShift = 4;

constexpr uint8_t kTemporalMask = 0x07;
constexpr unsigned kSpatialShift = 3;
constexpr uint8_t kReservedLayerBits = 0xE0;

static_assert(FrameReferences::kCapacity <= 0xF, "the reference count has four bits");
static_assert(FrameInfo::kMaxTemporalLayer <= kTemporalMask);
static_assert(FrameInfo::kMaxSpatialLayer << kSpatialShift <= 0xFF - kReservedLayerBits);

uint64_t capture_us(const FrameInfo& info) noexcept {
    return static_cast<uint64_t>(info.capture_time.time_since_epoch().count());
}

bool has_layers(const FrameInfo& info) noexcept {
    return info.temporal_layer != 0 || info.spatial_layer != 0;
}

}  // namespace

bool is_encodable(const FrameInfo& info) noexcept {
    if (info.capture_time.time_since_epoch().count() < 0) return false;
    if (info.temporal_layer > FrameInfo::kMaxTemporalLayer) return false;
    if (info.spatial_layer > FrameInfo::kMaxSpatialLayer) return false;
    for (FrameNumber reference : info.references) {
        if (reference >= info.number) return false;
    }
    return true;
}

size_t frame_header_size(const FrameInfo& info) noexcept {
    size_t size =
        2 + varint_size(info.stream) + varint_size(info.number) + varint_size(capture_us(info));
    if (has_layers(info)) size += 1;
    for (FrameNumber reference : info.references) size += varint_size(info.number - reference);
    return size;
}

Result<Buffer> encode_frame(const FrameInfo& info, MutableBuffer payload) {
    if (!is_encodable(info)) return fail(Errc::InvalidArgument);

    const size_t header_size = frame_header_size(info);
    if (payload.headroom() < header_size) {
        MutableBuffer moved = allocate_frame_payload(payload.size());
        moved.append(payload.bytes());
        payload = std::move(moved);
    }

    uint8_t flags = static_cast<uint8_t>(info.references.size() << kReferenceCountShift);
    if (info.keyframe) flags |= kKeyframeFlag;
    if (has_layers(info)) flags |= kLayersFlag;

    Writer out(std::span(payload.prepend(header_size), header_size));
    out.u8(message_prefix(MessageKind::Frame));
    out.u8(flags);
    out.varint(info.stream);
    out.varint(info.number);
    out.varint(capture_us(info));
    if (has_layers(info)) {
        out.u8(static_cast<uint8_t>(info.temporal_layer | info.spatial_layer << kSpatialShift));
    }
    for (FrameNumber reference : info.references) out.varint(info.number - reference);
    assert(out.position() == header_size);

    return std::move(payload).freeze();
}

Result<EncodedFrame> decode_frame(const Buffer& message) {
    Reader in(message.bytes());

    const Result<MessageKind> kind = read_prefix(in);
    if (!kind) return fail(kind.error());
    if (*kind != MessageKind::Frame) return fail(Errc::UnknownKind);

    const uint8_t flags = in.u8();
    if (flags & kReservedFlags) in.fail(Errc::Malformed);

    FrameInfo info;
    info.keyframe = (flags & kKeyframeFlag) != 0;

    const uint64_t stream = in.varint();
    if (stream > std::numeric_limits<StreamId>::max()) in.fail(Errc::Malformed);
    info.stream = static_cast<StreamId>(stream);

    info.number = in.varint();

    const uint64_t capture = in.varint();
    if (capture > static_cast<uint64_t>(std::numeric_limits<Duration::rep>::max())) {
        in.fail(Errc::Malformed);
    }
    info.capture_time = Timestamp{Duration{static_cast<Duration::rep>(capture)}};

    if (flags & kLayersFlag) {
        const uint8_t layers = in.u8();
        // All zero is what an absent layers byte means; sent, it is a second
        // encoding of the same frame.
        if ((layers & kReservedLayerBits) || layers == 0) in.fail(Errc::Malformed);
        info.temporal_layer = layers & kTemporalMask;
        info.spatial_layer = static_cast<uint8_t>(layers >> kSpatialShift);
    }

    const size_t reference_count = flags >> kReferenceCountShift;
    if (reference_count > FrameReferences::kCapacity) in.fail(Errc::Malformed);
    for (size_t i = 0; i < reference_count && in.ok(); ++i) {
        const uint64_t delta = in.varint();
        if (delta == 0 || delta > info.number) {
            in.fail(Errc::Malformed);
            break;
        }
        info.references.push_back(info.number - delta);
    }

    if (flags & kExtensionsFlag) {
        // Compared as 64 bits before skipping, so a huge length cannot narrow
        // into a small one where size_t is 32 bits.
        const uint64_t length = in.varint();
        if (length > in.remaining()) in.fail(Errc::Truncated);
        in.skip(static_cast<size_t>(length));
    }

    if (!in.ok()) return fail(in.error());
    return EncodedFrame{info, message.slice(in.position())};
}

}  // namespace ratcast::wire
