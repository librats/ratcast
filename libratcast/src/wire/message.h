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

#include "ratcast/result.h"
#include "wire/bytes.h"

namespace ratcast::wire {

inline constexpr uint8_t kWireVersion = 0;

enum class MessageKind : uint8_t {
    Frame = 1,  ///< an encoded frame, sent live (frame_codec.h)
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
        case MessageKind::Frame: return kind;
    }
    return fail(Errc::UnknownKind);
}

/// The kind of `message`, to dispatch on before decoding it.
inline Result<MessageKind> peek_kind(std::span<const uint8_t> message) noexcept {
    Reader in(message);
    return read_prefix(in);
}

}  // namespace ratcast::wire
