#pragma once

/**
 * @file chat_files.h
 * @brief The files of what is said in a call: what each is known by, and how
 * a call has them.
 *
 * A message carries files as ChatAttachments (chat.h): each its name, its
 * size and the id its bytes are known by — their BLAKE2b hash, file_id() —
 * with an image's size and a small picture of it. The bytes go apart from
 * the message. A member that wants a file asks one that may have it — who
 * said it first, then each other member in turn (MediaSession::ask_file()) —
 * and that one sends it through librats' FileTransfer, on a lane of its own
 * below every other: a file never holds up what is played. Or it says it has
 * none, and the next is asked. What comes is checked against the id before
 * it is kept, and a member that has a file sends it on in turn to whoever
 * asks: a file can be had while anyone who has it is in the room.
 * (Call::say(), Call::fetch_file().)
 */

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ratcast/core/result.h"
#include "ratcast/session/chat.h"

namespace ratcast {

/// A file said with a message: as the message describes it, and where this
/// side has its bytes (UTF-8).
struct SharedFile {
    ChatAttachment attachment;
    std::string path;
};

/// The id of the bytes of the file at `path` (UTF-8), read through: Unavailable
/// for a file that cannot be read, Cancelled once `stop` is set.
Result<FileId> file_id(const std::string& path, const std::atomic<bool>* stop = nullptr);
/// The id of `bytes`.
FileId file_id(std::span<const uint8_t> bytes) noexcept;

/// The name a file another named `name` is saved under here — a name any
/// system takes: what Windows has no file name with made '_' (<>:"|?*, and
/// ':' would name a stream of another file), no dot or space at its end, and
/// a device's name (CON, NUL, COM1, …) never its whole stem. A valid
/// attachment's name (is_valid()) has no folder in it already.
std::string local_file_name(std::string name);

/// `id` in lowercase hex, and back: none for anything but 64 hex digits.
std::string to_hex(const FileId& id);
std::optional<FileId> file_id_of_hex(std::string_view hex) noexcept;

}  // namespace ratcast
