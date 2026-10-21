#pragma once

/**
 * @file data_publisher.h
 * @brief An MPEG-TS feed published as a data stream: input → frames → session.
 *
 * The publisher finds the 188-byte packets in what the input reads — a packet
 * cut across two reads waits for its rest, and bytes that are no packet are
 * skipped until the next sync byte — and sends them six to a frame: as many as
 * fit one librats datagram, so that a lost datagram costs those packets and no
 * more.
 *
 * Frames go full. A feed comes in reads that seldom end on a frame's end —
 * over UDP, datagrams of seven packets — and sending each read whole would
 * take two half-empty datagrams for every one that came: twice the packets,
 * the acknowledgements and the system calls at both ends. So the packets that
 * do not fill a frame wait for the next read, and go as they are once the
 * input has been quiet for `hold`, when it ends, or at stop(). A frame's
 * capture time is when its first packet was read, which is what the receiver
 * plays it out by: what waited is not late.
 *
 * It runs on the input's thread: reading and sending take microseconds.
 */

#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/ts_io.h"
#include "ratcast/session/media_session.h"

namespace ratcast {

struct DataPublisherConfig {
    StreamId stream = 3;
    std::string label = "ts";
    /// How long a frame is worth delivering: what repair has to work in —
    /// SRT's latency, librist's buffer.
    Duration lifetime = std::chrono::milliseconds(200);
    /// Packets to a frame: six, 1128 bytes, fill a librats datagram.
    size_t packets_per_frame = 6;
    /// How long the packets that do not fill a frame may wait for more once
    /// the input is quiet: a bound on the delay a frame's last packets add to
    /// its first.
    Duration hold = std::chrono::milliseconds(5);
};

struct DataPublisherStats {
    uint64_t bytes_read = 0;
    uint64_t packets = 0;  ///< found in them
    uint64_t skipped = 0;  ///< bytes that were no packet
    uint64_t frames = 0;   ///< handed to the session
    uint64_t errors = 0;   ///< frames the session refused
};

class DataPublisher {
public:
    /// Once, when publishing stops by itself: with success when the input
    /// ended, the failure otherwise.
    using EndHandler = std::function<void(Result<void> outcome)>;

    /// `session` must outlive the publisher.
    DataPublisher(MediaSession& session, DataPublisherConfig config);
    ~DataPublisher();
    DataPublisher(const DataPublisher&) = delete;
    DataPublisher& operator=(const DataPublisher&) = delete;

    /// Announce the stream and publish what `input` reads until stop(). The
    /// input must stay alive until then; `ended` (may be empty) must not call
    /// stop(). Fails if already started, or with the session's or the input's
    /// own failure.
    Result<void> start(TsInput& input, EndHandler ended = {});

    /// Stop the input, send what it left held, and withdraw the stream.
    void stop();

    DataPublisherStats stats() const;

private:
    void take(std::span<const uint8_t> bytes, Timestamp received);
    void send_held();

    MediaSession& session_;
    const DataPublisherConfig config_;

    std::mutex lifecycle_;  ///< serialises start() and stop()
    TsInput* input_ = nullptr;

    // The input thread's own (stop()'s, once that has stopped).
    TsPacketFinder finder_;
    MutableBuffer held_;      ///< a frame not yet full
    Timestamp held_since_{};  ///< when its first packet was read
    FrameNumber next_number_ = 0;

    mutable std::mutex stats_mutex_;
    DataPublisherStats stats_;
};

}  // namespace ratcast
