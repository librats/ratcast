#pragma once

/**
 * @file data_player.h
 * @brief A subscribed data stream played out: session → DeliveryBuffer →
 * output.
 *
 * Frames arrive on a network thread as they come and go into a
 * DeliveryBuffer; a thread of the player's own writes each to the output when
 * it is due — in order, evenly, a fixed latency after it was read on the
 * sender's side — and skips what did not come in time. What comes out is the
 * feed that went in, as librist or SRT would hand it on.
 *
 * The writer sleeps until the next frame is due and is woken early only by a
 * frame due sooner than that; frames due together (a burst read at once, or
 * a repair that held back those behind it) go out in one write.
 */

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/ts_io.h"
#include "ratcast/session/media_session.h"
#include "ratcast/stream/delivery_buffer.h"
#include "ratcast/stream/frame.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast {

struct DataPlayerConfig {
    /// How long past the fastest frames each is held, for repair to bring the
    /// missing ones. nullopt: the stream's lifetime, past which none comes.
    std::optional<Duration> latency{};
};

struct DataPlayerStats {
    DeliveryBuffer::Stats delivery;  ///< in order, lost, late
    uint64_t bytes = 0;              ///< written out
    uint64_t write_errors = 0;
    Duration latency{};  ///< held past the fastest
};

class DataPlayer {
public:
    /// Plays `peer`'s stream `description.id` on `session` into `output`; both
    /// must outlive the player, and `output` is the player's alone while it
    /// plays.
    DataPlayer(MediaSession& session, PeerId peer, StreamDescription description, TsOutput& output,
               DataPlayerConfig config = {});
    ~DataPlayer();
    DataPlayer(const DataPlayer&) = delete;
    DataPlayer& operator=(const DataPlayer&) = delete;

    /// Subscribe and start playing out. Fails with InvalidArgument if already
    /// started, for a stream that is not data, or when the session refuses
    /// the subscription.
    Result<void> start();

    /// Unsubscribe and stop; nothing is written once this returns.
    void stop();

    /// The sender withdrew the stream: write out what is held, each at its
    /// time, and stop then. Does not wait for that: stop() or the destructor
    /// does, at most the latency.
    void finish();

    /// A frame of the stream, from MediaSession's frame_received. Never blocks
    /// for long.
    void push(EncodedFrame frame);

    const PeerId& peer() const noexcept { return peer_; }
    StreamId stream() const noexcept { return description_.id; }
    DataPlayerStats stats() const;

private:
    void run();
    /// The player's thread, no lock held.
    void write(std::span<const EncodedFrame> frames, std::vector<uint8_t>& joined);

    MediaSession& session_;
    const PeerId peer_;
    const StreamDescription description_;
    TsOutput& output_;
    const Duration latency_;

    std::mutex lifecycle_;  ///< serialises start() and stop()
    std::thread thread_;

    mutable std::mutex mutex_;  ///< the buffer, between network and player
    std::condition_variable wake_;
    DeliveryBuffer buffer_;
    bool running_ = false;
    bool finishing_ = false;  ///< nothing more comes: stop once all is written
    /// When the writer wakes by itself, while it sleeps; max: only when woken.
    std::optional<Timestamp> sleeping_until_;
    uint64_t bytes_ = 0;
    uint64_t write_errors_ = 0;
};

}  // namespace ratcast
