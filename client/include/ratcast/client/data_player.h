#pragma once

/**
 * @file data_player.h
 * @brief A subscribed data stream played out: session → DeliveryThread →
 * output.
 *
 * Frames arrive on a network thread as they come and go into a
 * DeliveryThread, whose thread writes each to the output when it is due — in
 * order, evenly, a fixed latency after it was read on the sender's side — and
 * skips what did not come in time. What comes out is the feed that went in, as
 * librist or SRT would hand it on. Frames due together go out in one write.
 */

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/ts_io.h"
#include "ratcast/session/media_session.h"
#include "ratcast/stream/delivery_buffer.h"
#include "ratcast/stream/delivery_thread.h"
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
    /// The delivery thread's.
    void write(std::span<const EncodedFrame> frames);

    MediaSession& session_;
    const PeerId peer_;
    const StreamDescription description_;
    TsOutput& output_;
    const Duration latency_;

    std::mutex lifecycle_;  ///< serialises start() and stop()
    bool started_ = false;

    std::vector<uint8_t> joined_;  ///< the delivery thread's: frames due together
    mutable std::mutex stats_mutex_;
    uint64_t bytes_ = 0;
    uint64_t write_errors_ = 0;

    /// Last, so that it stops before what its thread uses goes.
    DeliveryThread delivery_;
};

}  // namespace ratcast
