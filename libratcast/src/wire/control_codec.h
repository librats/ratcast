#pragma once

/**
 * @file control_codec.h
 * @brief The control plane: offering streams, subscribing, asking for keyframes.
 *
 * Control messages travel reliably and in order on the media lane, frames
 * beside them as live messages. Every one names a stream by the id its sender
 * gave it.
 *
 *   Announce          prefix, varint stream, u8 codec, u8 media kind,
 *                     varint lifetime (µs), varint label length + label,
 *                     video: varint width, height, frame rate num, den
 *                     audio: varint sample rate, u8 channels
 *                     varint codec config length + codec config
 *   Withdraw          prefix, varint stream
 *   Subscribe         prefix, varint stream
 *   Unsubscribe       prefix, varint stream
 *   KeyframeRequest   prefix, varint stream
 *
 * A later version may append fields to any of them: a receiver reads the
 * fields it knows and ignores whatever follows.
 */

#include <span>
#include <variant>

#include "ratcast/core/buffer.h"
#include "ratcast/core/result.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast::wire {

struct Announce {
    StreamDescription description;
};

struct Withdraw {
    StreamId stream = 0;
};

struct Subscribe {
    StreamId stream = 0;
};

struct Unsubscribe {
    StreamId stream = 0;
};

struct KeyframeRequest {
    StreamId stream = 0;
};

using ControlMessage = std::variant<Announce, Withdraw, Subscribe, Unsubscribe, KeyframeRequest>;

/// The message for `message`. Precondition: an Announce carries a description
/// for which is_valid() holds.
Buffer encode_control(const ControlMessage& message);

/// The control message in `message`. Fails with Unsupported for a stream of a
/// media kind this build does not know, and Malformed for an announced
/// description that is not valid.
Result<ControlMessage> decode_control(std::span<const uint8_t> message);

}  // namespace ratcast::wire
