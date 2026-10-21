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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"

namespace ratcast {

inline constexpr size_t kTsPacketSize = 188;
inline constexpr uint8_t kTsSyncByte = 0x47;

/// The packets in a feed read in pieces of any size: a packet cut across two
/// reads waits for its rest, and bytes that are no packet are skipped up to
/// the next sync byte.
class TsPacketFinder {
public:
    /// Hand `found(std::span<const uint8_t>)` the whole packets read so far,
    /// in runs that follow on in memory, valid only during the call. Returns
    /// the bytes skipped.
    template <typename Found>
    uint64_t take(std::span<const uint8_t> bytes, Found&& found);

private:
    std::vector<uint8_t> pending_;  ///< read, not yet handed on: a packet's start
};

/// Where a feed comes from, read on a thread of its own.
class TsInput {
public:
    /// Bytes as they came, `received` on the input's clock; valid only during
    /// the call. No bytes: the input has gone quiet (see start()).
    using Sink = std::function<void(std::span<const uint8_t> bytes, Timestamp received)>;

    /// Once, when the input ends by itself: with success at the end of a file
    /// or a pipe, the failure otherwise.
    using EndHandler = std::function<void(Result<void> outcome)>;

    virtual ~TsInput() = default;

    /// Start reading into `sink`; `ended` may be empty. Neither may call
    /// stop(). Fails if already started.
    ///
    /// Once nothing has come for `quiet` after some bytes did, `sink` is told
    /// so, once, with no bytes: a reader that holds part of what came, for the
    /// rest to follow, lets it go then. Zero: never told.
    virtual Result<void> start(Sink sink, EndHandler ended, Duration quiet) = 0;

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

template <typename Found>
uint64_t TsPacketFinder::take(std::span<const uint8_t> bytes, Found&& found) {
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    const auto begin = pending_.begin();
    size_t at = 0;
    size_t run = 0;  // where the packets not yet handed on start
    uint64_t skipped = 0;
    while (pending_.size() - at >= kTsPacketSize) {
        if (pending_[at] == kTsSyncByte) {
            at += kTsPacketSize;
            continue;
        }
        if (at > run) found(std::span<const uint8_t>(pending_.data() + run, at - run));
        const size_t next = static_cast<size_t>(
            std::find(begin + static_cast<ptrdiff_t>(at) + 1, pending_.end(), kTsSyncByte) - begin);
        skipped += next - at;
        at = run = next;
    }
    if (at > run) found(std::span<const uint8_t>(pending_.data() + run, at - run));
    pending_.erase(begin, begin + static_cast<ptrdiff_t>(at));
    return skipped;
}

}  // namespace ratcast
