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
 *
 * A message may carry files (ChatAttachment), each described in it — its
 * name, its size, the id its bytes are known by, an image's size and a small
 * picture of it — the bytes themselves asked of a member that has them
 * (MediaSession::ask_file()) and sent apart from the session.
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "ratcast/export.h"
#include "ratcast/session/media_network.h"

namespace ratcast {

/// A moment by the wall clock: microseconds since the Unix epoch.
using WallTime = std::chrono::sys_time<std::chrono::microseconds>;

/// What a file's bytes are known by, the same whoever has them: a hash of
/// them (the client's: BLAKE2b's, of 32 bytes).
using FileId = std::array<uint8_t, 32>;

/// A file a message carries, as the message describes it: what to ask for it
/// by, and what to show of it before it is here.
struct ChatAttachment {
    /// The longest name, in bytes of UTF-8.
    static constexpr size_t kMaxNameSize = 255;
    /// The largest picture of it: a few kilobytes, told to every member and,
    /// with the message, to each who comes later.
    static constexpr size_t kMaxPreviewSize = 4096;
    /// The largest side an image may say it has.
    static constexpr uint32_t kMaxImageSide = 65535;

    FileId id{};
    /// Its name, with no folder.
    std::string name{};
    uint64_t size = 0;
    /// An image's size in pixels, both or neither: neither, a file that is
    /// no image (or none shown as one).
    uint32_t width = 0;
    uint32_t height = 0;
    /// A small picture of it, encoded (JPEG); maybe none.
    std::vector<uint8_t> preview{};

    bool image() const noexcept { return width != 0 && height != 0; }

    friend bool operator==(const ChatAttachment&, const ChatAttachment&) = default;
};

struct ChatMessage {
    /// The longest text, in bytes of UTF-8: 1000 to 4000 characters.
    static constexpr size_t kMaxTextSize = 4000;
    /// The most files one message carries.
    static constexpr size_t kMaxAttachments = 10;

    PeerId author{};     ///< who said it
    std::string name{};  ///< what it called itself as it said it (Presence::name)
    WallTime sent{};     ///< when, by its clock
    /// UTF-8, not empty but in a message of files; line breaks and tabs the
    /// only control characters.
    std::string text{};
    std::vector<ChatAttachment> attachments{};

    friend bool operator==(const ChatMessage&, const ChatMessage&) = default;
};

/// Whether `attachment` can be told: a name of at most kMaxNameSize bytes of
/// valid UTF-8 — not empty, not "." or "..", without control characters,
/// slashes or backslashes; an image's sides both or neither, each at most
/// kMaxImageSide; a picture of at most kMaxPreviewSize bytes.
RATCAST_EXPORT bool is_valid(const ChatAttachment& attachment) noexcept;

/// Whether `message` can be told: a text of at most kMaxTextSize bytes of
/// valid UTF-8, without control characters but line breaks and tabs, and not
/// empty but in a message of files; a name as a Presence's; said after the
/// epoch; at most kMaxAttachments files, each valid, none twice. (A build
/// from before files reads a message of files as its text alone: one with
/// none is lost on it.)
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
