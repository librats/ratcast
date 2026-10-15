#pragma once

/**
 * @file frame_codec.h
 * @brief A frame on the wire: a compact header, then the payload as is.
 *
 * One frame is one live message (librats SendOptions::lifetime): the transport
 * delivers it whole or not at all, so the header needs no fragmentation fields
 * and the payload runs to the end of the message.
 *
 *   u8      prefix      wire version | MessageKind::Frame (message.h)
 *   u8      flags       bit 0     keyframe
 *                       bit 1     a layers byte follows the capture time
 *                       bit 2     an extension block follows the references
 *                       bit 3     reserved, 0
 *                       bits 4-7  number of references, 0..FrameReferences::kCapacity
 *   varint  stream
 *   varint  frame number
 *   varint  capture time, microseconds on the sender's monotonic clock
 *  [u8      layers      bits 0-2 temporal, bits 3-4 spatial, bits 5-7 reserved 0]
 *   varint  reference delta (frame number − reference, ≥ 1), once per reference
 *  [varint  extension block length, then that many bytes]
 *   …       payload
 *
 * A delta frame of a single-layer stream spends 10-15 bytes on all of this.
 *
 * The layers byte is present exactly when a layer is not 0. The extension block
 * is room for fields added later without a version change: this version skips
 * it, and never writes one.
 *
 * Every frame has exactly one encoding. A decoder refuses what no encoder
 * writes — reserved bits, non-minimal varints, an all-zero layers byte — so
 * re-encoding a decoded frame gives back the bytes it came from.
 */

#include <cstddef>
#include <span>

#include "ratcast/core/buffer.h"
#include "ratcast/core/result.h"
#include "ratcast/stream/frame.h"
#include "wire/bytes.h"

namespace ratcast::wire {

/// The largest frame header: prefix, flags, a 32-bit stream id, a 64-bit frame
/// number and capture time, the layers byte and the most references with the
/// widest deltas.
inline constexpr size_t kMaxFrameHeaderSize = 1 + 1 + varint_size(UINT32_MAX) + 2 * kMaxVarintSize +
                                              1 + FrameReferences::kCapacity * kMaxVarintSize;
static_assert(kMaxFrameHeaderSize <= kFrameHeadroom, "kFrameHeadroom must hold any frame header");

/// Whether `info` can be encoded: layers in range, a capture time not before
/// the clock's epoch, every reference earlier than the frame itself.
bool is_encodable(const FrameInfo& info) noexcept;

/// Size of the header encode_frame() writes for `info`.
/// Precondition: is_encodable(info).
size_t frame_header_size(const FrameInfo& info) noexcept;

/// The frame message for `info` and `payload`. The header goes into the
/// payload's headroom, so a payload from allocate_frame_payload() is sent
/// without being copied; one with too little headroom is copied once.
/// Fails with InvalidArgument unless is_encodable(info).
Result<Buffer> encode_frame(const FrameInfo& info, MutableBuffer payload);

struct FrameHeader {
    FrameInfo info;
    size_t size = 0;  ///< bytes of header; the payload starts here
};

/// The header of the frame message `message`, read in place: enough to decide
/// whether the frame is wanted before copying any of it.
Result<FrameHeader> read_frame_header(std::span<const uint8_t> message);

/// The frame in `message`, whose payload is a slice of `message` (no copy).
/// Expects the whole message, prefix included.
Result<EncodedFrame> decode_frame(const Buffer& message);

}  // namespace ratcast::wire
