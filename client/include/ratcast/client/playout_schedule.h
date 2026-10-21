#pragma once

/**
 * @file playout_schedule.h
 * @brief When to show each picture of a stream, so that late frames do not
 * show as stalls.
 *
 * Frames arrive unevenly. On a lossy path a piece that forward error
 * correction could not rebuild is sent again, a round trip later, and the
 * frames behind it, there already, wait for it in decode order: shown as they
 * come, each such run of late frames is a stall. And a transport that sends
 * once per round trip, as a congestion window does, delivers frames in clumps
 * a few tens of milliseconds apart: shown as they come, motion judders. Shown
 * each at its capture time plus a delay that covers how late frames come,
 * they play evenly, that delay later.
 *
 * The capture times are the sender's clock, so how late a frame is counts
 * from the fastest frames of the last 10–20 seconds (arrival less capture
 * time: the clocks' offset plus the path's least delay). Late frames come in
 * runs, up to a frame on time; the delay covers the second latest run of the
 * last ten seconds. One run alone is a stall whatever the delay — the delay
 * can only cover the next — and covering the latest would hold every picture
 * for the one worst moment of ten seconds; a run as late again says that the
 * path does it. Left out are runs within moments of a keyframe — it took
 * long to send and the frames behind it waited; it comes with a new bitrate,
 * whose encoder slowed the one at work while it opened and is slow with its
 * first frames; or it starts the stream over after a loss: a stall now and
 * then that a delay would make every picture pay for — and runs later than
 * `max_delay`, past which a frame is better shown late than every picture
 * held for it.
 *
 * The delay grows at once; it shrinks, as late runs age out of the ten
 * seconds, by 5% of the time passing: pictures play that much faster
 * meanwhile, which nobody sees.
 *
 * Not thread-safe.
 */

#include <deque>
#include <optional>

#include "ratcast/core/time.h"

namespace ratcast {

class PlayoutSchedule {
public:
    /// Pictures are held for at most `max_delay` past the fastest; zero shows
    /// every picture as soon as it is there.
    explicit PlayoutSchedule(Duration max_delay) noexcept : max_delay_(max_delay) {}

    void set_max_delay(Duration max_delay) noexcept;

    /// A frame captured at `capture` (the sender's clock) arrived at `arrival`
    /// (this clock).
    void on_frame(Timestamp capture, Timestamp arrival, bool keyframe = false);

    /// When to show a picture captured at `capture`, on this clock; at once
    /// before any frame arrived.
    Timestamp due(Timestamp capture) const noexcept;

    /// How much later than the fastest frames pictures are shown now.
    Duration delay() const noexcept { return delay_; }

    /// From capture to due, now: the fastest transit and the delay; zero
    /// before any frame arrived.
    Duration offset() const noexcept { return fastest_ ? fastest() + delay_ : Duration::zero(); }

    /// Forget everything: a new stream, or the same one starting over.
    void reset() noexcept;

private:
    /// The least of `arrival - capture` over the last one to two windows.
    struct Fastest {
        Duration current{};
        Timestamp since{};
        std::optional<Duration> previous;
    };
    /// A run of late frames: how late the latest was, and when it ended.
    struct Run {
        Duration late{};
        Timestamp at{};
        bool keyframe = false;  ///< with a keyframe in it, or one just before
    };

    Duration fastest() const noexcept;
    /// What the delay should be now: the second latest run that counts.
    Duration wanted() const noexcept;

    Duration max_delay_;
    std::optional<Fastest> fastest_;
    std::deque<Run> runs_;                    ///< ended within the window, oldest first
    std::optional<Run> run_;                  ///< under way
    std::optional<Timestamp> last_keyframe_;  ///< arrived
    Duration delay_{};
    Timestamp last_arrival_{};
};

}  // namespace ratcast
