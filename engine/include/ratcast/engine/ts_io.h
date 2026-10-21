#pragma once

/**
 * @file ts_io.h
 * @brief MPEG-TS feeds in and out: what librist and SRT carry, and what
 * encoders (ffmpeg, OBS, hardware) and players speak.
 *
 * ratcast carries such a feed as a data stream, without looking inside, so
 * these only move bytes, in whole 188-byte packets or not: a reader that
 * frames them (the client's DataPublisher) finds the packets.
 *
 * Where a feed comes from and goes to is a URL:
 *
 *   - `udp://host:port`: datagrams. In, those sent to `port` on `host`, which
 *     may be omitted (`udp://:port`, or ffmpeg's `udp://@:port`) and may be a
 *     multicast group, which is joined; out, datagrams of up to seven packets
 *     to `host:port`.
 *   - `-`: standard input or output, as fast as it comes: a pipe from or to
 *     ffmpeg.
 *   - a file path: in, played in real time by the PCR it carries, as a live
 *     encoder would send it; out, written as it comes.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"

namespace ratcast {

inline constexpr size_t kTsPacketSize = 188;
inline constexpr uint8_t kTsSyncByte = 0x47;

/// Where a feed comes from, read on a thread of its own.
class TsInput {
public:
    /// Bytes as they came, `received` on the input's clock; valid only during
    /// the call.
    using Sink = std::function<void(std::span<const uint8_t> bytes, Timestamp received)>;

    /// Once, when the input ends by itself: with success at the end of a file
    /// or a pipe, the failure otherwise.
    using EndHandler = std::function<void(Result<void> outcome)>;

    virtual ~TsInput() = default;

    /// Start reading into `sink`; `ended` may be empty. Neither may call
    /// stop(). Fails if already started.
    virtual Result<void> start(Sink sink, EndHandler ended) = 0;

    /// Stop. When this returns, neither handler is running or will be called
    /// again.
    virtual void stop() = 0;
};

/// Where a feed goes.
class TsOutput {
public:
    virtual ~TsOutput() = default;

    /// Write `bytes`, whole packets, on whichever thread calls; one at a time.
    virtual Result<void> write(std::span<const uint8_t> bytes) = 0;
};

/// An input for `url`, ready to start. Fails with InvalidArgument for a URL of
/// none of the forms above, Unavailable when the port cannot be bound or the
/// file opened.
Result<std::unique_ptr<TsInput>> open_ts_input(std::string_view url,
                                               Clock& clock = SteadyClock::instance());

/// An output for `url`. Fails as open_ts_input() does.
Result<std::unique_ptr<TsOutput>> open_ts_output(std::string_view url);

}  // namespace ratcast
