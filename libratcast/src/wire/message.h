#pragma once

/**
 * @file message.h
 * @brief The byte every libratcast message starts with.
 *
 *   bits 5-7  wire version
 *   bits 0-4  message kind
 *
 * A receiver checks the version before reading anything else, so a message
 * from an incompatible build is refused for what it is instead of being
 * misread. Minor additions do not need a new version: frame messages carry an
 * extension block an older receiver skips (see frame_codec.h).
 */

#include <cstdint>
#include <span>

#include "ratcast/core/result.h"
#include "wire/bytes.h"

namespace ratcast::wire {

inline constexpr uint8_t kWireVersion = 0;

enum class MessageKind : uint8_t {
    Frame = 1,            ///< an encoded frame, sent live (frame_codec.h)
    Announce = 2,         ///< a stream is offered, or its description changed (control_codec.h)
    Withdraw = 3,         ///< a stream is no longer offered
    Subscribe = 4,        ///< send me this stream's frames
    Unsubscribe = 5,      ///< stop sending them
    KeyframeRequest = 6,  ///< I cannot decode until the next keyframe
    Presence = 7,         ///< who I am, and whether I am muted or deafened
    ClockProbe = 8,       ///< what is your clock? (playout in step)
    ClockEcho = 9,        ///< this, as your probe came
    PlayoutNeed = 10,     ///< what I need to play your share, capture to played
    PlayoutDelay = 11,    ///< what all your viewers play mine at
    Forward = 12,         ///< pass my stream's frames on to these viewers
    RelayedPath = 13,     ///< the path to a viewer I pass your frames on to
    RelayedFrame = 14,    ///< a frame of another's stream, passed on (frame_codec.h)
    Uplink = 15,          ///< my uplink was found full, carrying this; or is no longer known to be
    CannotReach = 16,     ///< I am not connected to this viewer you would have me pass on to
    Forwarder = 17,       ///< I watch nothing and pass on what I am handed: a server
    FrameKey = 18,        ///< the key my frames are sealed under (SFrame)
    Chat = 19,            ///< I say this, in writing
    ChatAsk = 20,         ///< what was said?
    ChatTold = 21,        ///< this was said, as I know it
};

constexpr uint8_t message_prefix(MessageKind kind) noexcept {
    return static_cast<uint8_t>(kWireVersion << 5 | static_cast<uint8_t>(kind));
}

/// Read the prefix and check it is one this build understands.
inline Result<MessageKind> read_prefix(Reader& in) noexcept {
    const uint8_t prefix = in.u8();
    if (!in.ok()) return fail(in.error());
    if (prefix >> 5 != kWireVersion) return fail(Errc::UnsupportedVersion);

    const auto kind = static_cast<MessageKind>(prefix & 0x1F);
    switch (kind) {
        case MessageKind::Frame:
        case MessageKind::Announce:
        case MessageKind::Withdraw:
        case MessageKind::Subscribe:
        case MessageKind::Unsubscribe:
        case MessageKind::KeyframeRequest:
        case MessageKind::Presence:
        case MessageKind::ClockProbe:
        case MessageKind::ClockEcho:
        case MessageKind::PlayoutNeed:
        case MessageKind::PlayoutDelay:
        case MessageKind::Forward:
        case MessageKind::RelayedPath:
        case MessageKind::RelayedFrame:
        case MessageKind::Uplink:
        case MessageKind::CannotReach:
        case MessageKind::Forwarder:
        case MessageKind::FrameKey:
        case MessageKind::Chat:
        case MessageKind::ChatAsk:
        case MessageKind::ChatTold: return kind;
    }
    return fail(Errc::UnknownKind);
}

/// The kind of `message`, to dispatch on before decoding it.
inline Result<MessageKind> peek_kind(std::span<const uint8_t> message) noexcept {
    Reader in(message);
    return read_prefix(in);
}

}  // namespace ratcast::wire
