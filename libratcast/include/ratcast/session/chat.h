#pragma once

/**
 * @file chat.h
 * @brief What members of a call say in writing, and what one of them knows
 * of it.
 *
 * A message goes from the member that says it to each peer over their
 * connection, whose Noise says who sent it (MediaSession::say()). One who
 * comes later asks each peer as it connects what was said, and each tells it
 * the last messages it knows: those it was told, as they were told it —
 * nothing proves who said them but the peer passing them on.
 *
 * A message is known by who said it and when, by the sayer's clock: told
 * twice, or by several, it is kept once.
 */

#include <chrono>
#include <cstddef>
#include <deque>
#include <string>

#include "ratcast/export.h"
#include "ratcast/session/media_network.h"

namespace ratcast {

/// A moment by the wall clock: microseconds since the Unix epoch.
using WallTime = std::chrono::sys_time<std::chrono::microseconds>;

struct ChatMessage {
    /// The longest text, in bytes of UTF-8: 1000 to 4000 characters.
    static constexpr size_t kMaxTextSize = 4000;

    PeerId author{};     ///< who said it
    std::string name{};  ///< what it called itself as it said it (Presence::name)
    WallTime sent{};     ///< when, by its clock
    std::string text{};  ///< UTF-8, not empty; line breaks and tabs the only control characters

    friend bool operator==(const ChatMessage&, const ChatMessage&) = default;
};

/// Whether `message` can be told: a text of at most kMaxTextSize bytes of
/// valid UTF-8, not empty, without control characters but line breaks and
/// tabs; a name as a Presence's; said after the epoch.
RATCAST_EXPORT bool is_valid(const ChatMessage& message) noexcept;

/// What one member knows was said: the last `kept` messages, in the order
/// they were said — by when, then by who, so that members who know the same
/// messages list them alike — each once.
class RATCAST_EXPORT ChatLog {
public:
    static constexpr size_t kDefaultKept = 100;

    explicit ChatLog(size_t kept = kDefaultKept);

    /// Keep `message`, the oldest let go past `kept`. False for one known
    /// already, or older than every one a full log keeps: nothing changed.
    bool add(ChatMessage message);

    /// Oldest first.
    const std::deque<ChatMessage>& messages() const noexcept { return messages_; }

private:
    size_t kept_;
    std::deque<ChatMessage> messages_;
};

}  // namespace ratcast
