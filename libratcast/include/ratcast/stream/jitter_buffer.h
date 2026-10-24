#pragma once

/**
 * @file jitter_buffer.h
 * @brief Frames of sound as they arrive, played as a steady stream.
 *
 * Sound has to be played without a break: a device asks for the next few
 * milliseconds whether or not the frame for them has come. A JitterBuffer
 * stands between the network and the decoder and decides, each time the
 * player needs more, what fills it: the next frame, or a gap the decoder makes
 * up — a loss, DTX's silence, or time bought for frames that come late.
 *
 * It plays the sender's timeline (capture times) behind the local clock by an
 * offset, which is the whole of its policy:
 *
 *   - A frame is in time if it arrives before the offset has passed since it
 *     was captured. How late frames arrive beyond the fastest of them (the
 *     path's base transit, which hides the clocks' difference) is kept as a
 *     histogram that forgets over `memory`; the *target* delay is the lateness
 *     a share `in_time` of frames keeps within, and one frame more — so that
 *     the frame after a lost one is there when the lost one is due, with the
 *     FEC that stands in for it — between min_delay and max_delay.
 *   - Frames are played by capture time. One not there when its time comes
 *     is made up; if it arrives after, it is dropped as late. A later frame
 *     there already means the missing ones are lost, and its FEC may stand for
 *     the last of them (the decoder's affair: the gap comes with that frame).
 *     Sound made up while nothing was there is a loss or DTX's silence, and
 *     counted as neither until the next frame says which.
 *   - The delay grows where sound is made up anyway — while the next frame is
 *     not there, which while short of target it waits for rather than give it
 *     up, until the delay is all of the target (no less, or the FEC after a
 *     loss may come too late), and in DTX's silence — and shrinks in silence
 *     too, by cutting it short.
 *     Only a delay well over target for a second together, in sound without
 *     silence, costs a frame: it is dropped.
 *   - A floor under the target (set_min_delay(): lip sync, when the pictures
 *     need longer than the sound) is pursued through sound too: a delay
 *     more than a frame short of it for a second together has a frame's worth
 *     made up every 100 ms until it is not, the drop's mirror image.
 *
 * Codec-agnostic: it reads frame numbers and capture times only. A gap in
 * numbers is a loss, a gap in time with none in numbers is silence (what the
 * engine's audio encoder sends; see audio_codec.h).
 *
 * Not thread-safe: the player locks around it.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ratcast/core/time.h"
#include "ratcast/export.h"
#include "ratcast/stream/fastest_transit.h"
#include "ratcast/stream/frame.h"

namespace ratcast {

class RATCAST_EXPORT JitterBuffer {
public:
    struct Config {
        /// The least delay kept beyond the base transit, however steady the path.
        Duration min_delay = std::chrono::milliseconds(10);
        /// The most: frames later than this are lost rather than waited for. The
        /// stream's lifetime, past which the network drops them anyway.
        Duration max_delay = std::chrono::milliseconds(150);
        /// The share of frames the target delay is to have in time.
        double in_time = 0.97;
        /// How long lateness is remembered: a spike stops raising the target
        /// about this long after it.
        Duration memory = std::chrono::seconds(10);
        /// Frames held at most; past it the oldest are dropped.
        size_t max_frames = 64;
    };

    struct Stats {
        uint64_t received = 0;  ///< frames pushed
        uint64_t played = 0;    ///< frames handed out to decode
        uint64_t late = 0;      ///< arrived after their time, or twice
        uint64_t lost = 0;      ///< frame numbers whose time passed before they came
        uint64_t dropped = 0;   ///< played over to bring the delay down, or for room
        Duration concealed{};   ///< made up for frames found lost
        Duration silence{};     ///< DTX's silence, as long as it was played
        Duration expanded{};    ///< added to raise the delay
        Duration cut{};         ///< taken out of silence to lower it
        /// Made up since the last frame played, while none was there: silence
        /// or a loss, which the next frame to come will tell (it goes to
        /// `silence` or `concealed` then).
        Duration unexplained{};
    };

    /// What to play next.
    struct Next {
        enum class Kind : uint8_t {
            Wait,   ///< nothing yet: before the first frame is due
            Frame,  ///< decode `frame`
            Gap,    ///< make up `duration`: no frame for it
        };
        Kind kind = Kind::Wait;
        /// Where it starts on the sender's timeline.
        Timestamp capture_time{};
        /// Of a gap: how much sound to make up, at most one frame's worth.
        Duration duration{};
        /// Of a frame: the frame. Of a gap that ends where frames were lost:
        /// the frame right after them, whose FEC may stand for the last one.
        std::optional<EncodedFrame> frame;
    };

    explicit JitterBuffer(Config config);

    /// Forget everything and start again with the next frame; the floor stays.
    void reset();

    /// Keep the target at `floor` at least (within max_delay); zero for none.
    void set_min_delay(Duration floor) noexcept;

    /// Accept `frame`, received at `now`.
    void push(EncodedFrame frame, Timestamp now);

    /// What to play next, for a player that has run out at `now`. Call again
    /// for more: each answer covers at most one frame's worth of sound.
    Next next(Timestamp now);

    /// The delay aimed at, beyond the base transit.
    Duration target() const noexcept { return target_; }

    /// The delay it would aim at with no floor: what the path asks for.
    Duration wanted() const noexcept { return wanted_; }

    /// The delay being played at, beyond the base transit, as of the last
    /// next(); zero before playing starts.
    Duration delay() const noexcept { return delay_; }

    /// The sender's frame duration, as learnt from consecutive capture times.
    Duration frame_duration() const noexcept { return frame_duration_; }

    const Stats& stats() const noexcept { return stats_; }

private:
    /// Lateness beyond the base transit, by kBucket, with old samples
    /// weighing less and less.
    static constexpr Duration kBucket = std::chrono::milliseconds(2);
    static constexpr size_t kBuckets = 256;  ///< up to half a second

    void learn(const EncodedFrame& frame, Timestamp now);
    void update_target();
    std::optional<Next> play_front(Timestamp now);
    bool stretch(Timestamp now);
    Next gap(Duration duration, std::optional<EncodedFrame> after, bool advance);

    Config config_;
    Stats stats_;

    std::vector<EncodedFrame> frames_;  ///< sorted by number, all >= next_number_

    FastestTransit fastest_;  ///< the base transit

    std::array<double, kBuckets> lateness_{};
    std::optional<Timestamp> last_arrival_;
    Duration target_;
    Duration wanted_;  ///< the target but for the floor
    Duration floor_{};

    Duration frame_duration_ = std::chrono::milliseconds(20);  ///< until learnt
    bool learnt_ = false;
    std::optional<FrameInfo> last_pushed_;

    // The cursor, once playing: where the sender's timeline is up to, and
    // the number of the next frame on it.
    bool playing_ = false;
    std::optional<Timestamp> first_arrival_;
    Timestamp cursor_{};
    FrameNumber next_number_ = 0;
    Duration delay_{};
    std::optional<Timestamp> over_since_;   ///< the delay has been well over target since
    std::optional<Timestamp> under_since_;  ///< and well under the floor since
};

}  // namespace ratcast
