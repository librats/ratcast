#pragma once

/**
 * @file frame_tracker.h
 * @brief Turns frames as they arrive into frames a decoder can take.
 *
 * Live messages arrive whole or not at all, but in no particular order: a frame
 * whose lost piece had to be repaired completes after the frame sent behind it.
 * A decoder, though, needs frames in decode order and every reference in place.
 * FrameTracker sits between the two:
 *
 *   - It hands frames on strictly in frame-number order. A frame that arrives
 *     behind a gap waits for the missing ones.
 *   - It waits no longer than `patience`, counted from the arrival of the first
 *     frame behind the gap. The missing frame was sent before that one, so its
 *     own lifetime ran out no later than that one's; past `patience` (the
 *     stream's lifetime) it is not late, it is lost, and the gap is skipped.
 *   - A frame whose references were all handed on is decodable. One with a
 *     reference that was lost is not: it is dropped, and the stream is
 *     *broken* until a keyframe. Losing a frame nothing references (a top
 *     temporal layer) breaks nothing.
 *   - A decodable keyframe is handed on at once, whatever gap is before it:
 *     nothing before a keyframe is needed by it or by anything after it.
 *   - While the stream is broken, or before its first keyframe, it asks for a
 *     keyframe, at most once per `keyframe_retry`. The receiver can break it
 *     too, when its decoder fails (invalidate()).
 *
 * Not thread-safe: one owner drives it (MediaSession locks around it).
 * Allocation-free once constructed, as long as ready frames are taken with
 * pop() after each push() or update().
 */

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ratcast/core/time.h"
#include "ratcast/export.h"
#include "ratcast/stream/frame.h"

namespace ratcast {

class RATCAST_EXPORT FrameTracker {
public:
    struct Config {
        /// Longest a frame waits behind a gap: the stream's lifetime.
        Duration patience = std::chrono::milliseconds(150);
        /// Least time between two keyframe requests.
        Duration keyframe_retry = std::chrono::milliseconds(250);
        /// Frames held waiting at most; past it the oldest gap is given up.
        size_t max_pending = 128;
    };

    struct Stats {
        uint64_t received = 0;     ///< frames pushed
        uint64_t delivered = 0;    ///< frames handed on to pop()
        uint64_t late = 0;         ///< arrived after the tracker had moved past them, or twice
        uint64_t superseded = 0;   ///< waiting when a later keyframe made them pointless
        uint64_t undecodable = 0;  ///< dropped for a lost reference, or with nothing to start from
        uint64_t lost = 0;         ///< frame numbers that never arrived and were given up
        uint64_t keyframe_requests = 0;
    };

    /// How far back references are remembered. A reference older than this
    /// (in frame numbers) counts as lost.
    static constexpr size_t kReferenceWindow = 1024;

    explicit FrameTracker(Config config);

    /// Start over, waiting for a keyframe: on subscribing. Counts as a
    /// keyframe request made at `now` — subscribing asks the sender for one.
    void reset(Timestamp now);

    /// Change the waiting limits; for a stream whose description changed.
    void reconfigure(Config config);

    /// Accept `frame`, received at `now`.
    void push(EncodedFrame frame, Timestamp now);

    /// Make the progress that is due by `now` without a new frame: give up
    /// gaps whose patience ran out.
    void update(Timestamp now);

    /// What was handed on is of no use (the decoder failed on it): the stream
    /// is broken until the next keyframe, and frames that reference anything
    /// before it are dropped. Counts as a keyframe request made at `now`.
    void invalidate(Timestamp now);

    /// The next frame to decode, in decode order; nullopt when none is ready.
    std::optional<EncodedFrame> pop();

    /// Whether to ask the sender for a keyframe now. True at most once per
    /// keyframe_retry, and only while the stream cannot be decoded.
    bool want_keyframe(Timestamp now) noexcept;

    /// Whether frames can currently be decoded: a keyframe was handed on and
    /// nothing since has been lost that the frames after it needed.
    bool decodable() const noexcept { return started_ && !broken_; }

    const Stats& stats() const noexcept { return stats_; }

private:
    struct Pending {
        EncodedFrame frame;
        Timestamp arrived;
    };

    void advance(Timestamp now);
    void make_room(Timestamp now);
    bool jump_to_keyframe();
    void deliver_front();
    void discard_front(uint64_t& counter);
    void skip_to(FrameNumber number);
    bool references_delivered(const FrameInfo& info) const noexcept;
    bool was_delivered(FrameNumber number) const noexcept;

    Config config_;
    Stats stats_;

    bool started_ = false;  ///< a keyframe has been handed on
    bool broken_ = false;   ///< a frame was dropped for a lost reference since then
    /// Once started: the last frame number dealt with, handed on or given up.
    /// Every pending frame is past it.
    FrameNumber last_ = 0;
    /// Which of the numbers (last_ - window, last_] were handed on, by number % window.
    std::bitset<kReferenceWindow> delivered_;

    std::vector<Pending> pending_;  ///< sorted by frame number, all > last_
    std::vector<EncodedFrame> ready_;
    size_t ready_head_ = 0;

    std::optional<Timestamp> last_request_;
};

}  // namespace ratcast
