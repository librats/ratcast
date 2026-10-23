#pragma once

/**
 * @file voice_meter.h
 * @brief How every voice in a room sounds to one member: a PlaybackMeter each.
 *
 * Told of each frame of sound heard (CallPlayback::heard) — whose, and its
 * latency from capture where known — it keeps for each peer what a
 * PlaybackMeter keeps for pictures: the wait for its first frame, breaks (gaps
 * past 100 ms, which a listener hears), latency. Every peer's wait counts from
 * start(), however late it joined: the wait for the last voice is how long the
 * room took to come together for this member.
 *
 * A voice that falls silent with DTX sends nothing, which counts as a break:
 * measure voices that keep speaking (a tone). Thread-safe.
 */

#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "ratcast/client/playback_meter.h"
#include "ratcast/core/time.h"
#include "ratcast/session/media_network.h"

namespace ratcast {

class VoiceMeter {
public:
    explicit VoiceMeter(Clock& clock = SteadyClock::instance(),
                        Duration break_after = std::chrono::milliseconds(100));

    /// Start measuring now: every voice is forgotten.
    void start();

    /// A frame of `peer`'s voice is heard now; `latency` from its capture, if
    /// known.
    void heard(const PeerId& peer, std::optional<Duration> latency = std::nullopt);

    /// Each voice since start(), by peer.
    std::map<PeerId, PlaybackStats> voices() const;
    /// Each voice since the previous call (or start()), then a new interval.
    std::map<PeerId, PlaybackStats> take_intervals();

    /// Voices together: frames, breaks and their time added up, the longest
    /// gap and the latency of them all; the first frame (in totals) when the
    /// last of them was first heard.
    static PlaybackStats together(const std::map<PeerId, PlaybackStats>& voices);

private:
    Clock& clock_;
    const Duration break_after_;
    mutable std::mutex mutex_;
    Timestamp started_;
    std::map<PeerId, std::unique_ptr<PlaybackMeter>> meters_;
};

}  // namespace ratcast
