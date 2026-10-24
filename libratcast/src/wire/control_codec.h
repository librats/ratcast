#pragma once

/**
 * @file control_codec.h
 * @brief The control plane: offering streams, subscribing, asking for keyframes,
 * saying who one is, agreeing when a share plays.
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
 *   PresenceUpdate    prefix, varint name length + name,
 *                     u8 flags (bit 0 muted, bit 1 deafened)
 *   ClockProbe        prefix, varint sent (µs, the prober's clock)
 *   ClockEcho         prefix, varint the probe's sent, varint at (µs, the
 *                     echoer's capture clock)
 *   PlayoutNeed       prefix, u8 1 + varint latency (µs), or u8 0: no longer
 *   PlayoutDelay      prefix, u8 1 + varint delay (µs), or u8 0: none
 *
 * A later version may append fields to any of them: a receiver reads the
 * fields it knows and ignores whatever follows.
 */

#include <optional>
#include <span>
#include <variant>

#include "ratcast/core/buffer.h"
#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/session/presence.h"
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

struct PresenceUpdate {
    Presence presence;
};

/// What is your clock? Answered with a ClockEcho at once.
struct ClockProbe {
    Timestamp sent{};  ///< the prober's clock, given back as it came
};

struct ClockEcho {
    Timestamp sent{};  ///< the probe's
    Timestamp at{};    ///< the echoer's clock — the one its frames are captured on
};

/// A viewer's word to a sender: what it needs to play the sender's share,
/// from capture to played, on the sender's clock; none: it plays the share in
/// step no more.
struct PlayoutNeed {
    std::optional<Duration> latency;
};

/// A sender's word to its viewers: the delay they all play its share at,
/// from capture, on its clock; none: each its own.
struct PlayoutDelay {
    std::optional<Duration> delay;
};

/// The most a PlayoutNeed or a PlayoutDelay may say.
inline constexpr Duration kMaxPlayout = std::chrono::seconds(10);

using ControlMessage =
    std::variant<Announce, Withdraw, Subscribe, Unsubscribe, KeyframeRequest, PresenceUpdate,
                 ClockProbe, ClockEcho, PlayoutNeed, PlayoutDelay>;

/// The message for `message`. Precondition: an Announce carries a description,
/// a PresenceUpdate a presence, for which is_valid() holds; a PlayoutNeed or a
/// PlayoutDelay says no more than kMaxPlayout, and nothing below zero.
Buffer encode_control(const ControlMessage& message);

/// The control message in `message`. Fails with Unsupported for a stream of a
/// media kind this build does not know, and Malformed for an announced
/// description or a presence that is not valid, or a latency or delay past
/// kMaxPlayout.
Result<ControlMessage> decode_control(std::span<const uint8_t> message);

}  // namespace ratcast::wire
