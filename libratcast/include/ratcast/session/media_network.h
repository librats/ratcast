#pragma once

/**
 * @file media_network.h
 * @brief The network as a MediaSession sees it: peers by id, two ways to send.
 *
 * MediaService implements it over a librats node; tests implement it in memory
 * and run sessions in virtual time. A future SFU can put the same session
 * behind whatever carries its traffic.
 */

#include <cstddef>
#include <cstdint>

#include "ratcast/core/buffer.h"
#include "ratcast/core/time.h"

#include "librats/peer/peer_id.h"

namespace ratcast {

using PeerId = librats::PeerId;

/// Which of a peer's live messages leave first (librats' SendOptions::rank),
/// whatever their deadlines: sound must not wait behind a keyframe's few hundred
/// packets, nor a keyframe behind the frames that depend on it.
enum class LiveRank : uint8_t {
    Sound = 0,
    Keyframe = 1,
    Frame = 2,
};

class MediaNetwork {
public:
    virtual ~MediaNetwork() = default;

    /// Send `message` to `peer` reliably, in order with every other reliable
    /// message to it.
    ///
    /// Implementations must not call back into the session from inside a send:
    /// the session may hold a stream's lock while sending.
    virtual void send_reliable(const PeerId& peer, const Buffer& message) = 0;

    /// Send `message` to `peer` live: delivered within `lifetime` of this call,
    /// whole, or not at all. Returns whether the peer has room for more; false
    /// means the message was queued but the sender must hold back until
    /// writable() says so. It leaves ahead of live messages of a later `rank`.
    ///
    /// What becomes of it — delivered, or given up on — goes to the session's
    /// on_live_outcome() under `tag` as soon as the network knows (librats'
    /// on_live_outcome), never from inside this call. Nothing for a message to
    /// a peer that disconnects first.
    virtual bool send_live(const PeerId& peer, const Buffer& message, Duration lifetime,
                           uint64_t tag, LiveRank rank) = 0;

    /// Whether `peer` can take more right now.
    virtual bool writable(const PeerId& peer) const = 0;

    /// Largest live message the network carries.
    virtual size_t max_live_message() const = 0;
};

}  // namespace ratcast
