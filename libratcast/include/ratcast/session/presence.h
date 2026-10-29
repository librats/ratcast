#pragma once

/**
 * @file presence.h
 * @brief Who a peer is in a call, as it says: a name, whether it is muted or
 * deafened, and the video codecs it decodes as they come.
 *
 * A session tells its own to every peer as it connects and again whenever it
 * changes (MediaSession::set_presence()); a receiver keeps the last each peer
 * told it. Nothing in it is checked beyond its form: a name is what the peer
 * calls itself.
 */

#include <cstddef>
#include <string>
#include <vector>

#include "ratcast/export.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast {

struct Presence {
    /// The longest name, in bytes of UTF-8: 32 characters of any script.
    static constexpr size_t kMaxNameSize = 128;

    std::string name;       ///< UTF-8 without control characters; may be empty
    bool muted = false;     ///< not heard: its microphone off
    bool deafened = false;  ///< hears nobody
    /// The video codecs it plays a share of as it comes — on a phone, those
    /// its hardware decodes — best first; what a sender's pictures to it are
    /// coded in follows them. Empty: not said (a server, which decodes
    /// nothing), which holds no sender to anything.
    std::vector<Codec> decodes = {};

    friend bool operator==(const Presence&, const Presence&) = default;
};

/// Whether `presence` can be told: a name of at most kMaxNameSize bytes of
/// valid UTF-8 without control characters, and video codecs, each once.
RATCAST_EXPORT bool is_valid(const Presence& presence) noexcept;

}  // namespace ratcast
