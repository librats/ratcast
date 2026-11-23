#pragma once

/**
 * @file size_adapter.h
 * @brief How many lines a shared screen is sent at: as many as the path and
 * the encoder carry.
 *
 * A screen of 4K squeezed into what a slow path carries is a smear in every
 * move; the same bits spent on 1080 lines show it well. And an encoder that
 * cannot keep up with 4K skips pictures: the stream stutters. So the size
 * steps down a level (2160, 1440, 1080, 720 lines — those below what the
 * pictures have) when either says so, and back up when the path has room:
 *
 *   - Down when the bitrate rate control aims at has stayed below what the
 *     size needs for three seconds (35% of bitrate_for_size()'s): straight to
 *     the largest size it keeps up, or the least.
 *   - Down a level when the encoder skipped more than a fifth of the pictures
 *     over two seconds: and never up past that level again, as the computer
 *     will not get faster.
 *   - Up a level once the bitrate has stayed at 60% of what it needs for ten
 *     seconds: twice as long after each step up that came straight back down
 *     (within twenty seconds), up to 160, back to ten after a minute held.
 *
 * Each change starts the encoder over with a keyframe, so after one nothing
 * changes for five seconds: rate control finds its feet first (it starts
 * low and ramps up ×1.5 a second, rate_controller.h). Sizes are told in
 * lines, as video sizes are named — the shorter side (fit_lines()).
 *
 * Pure logic in the time it is given: the publisher's encoder thread asks it
 * with each picture.
 */

#include <cstdint>
#include <optional>

#include "ratcast/core/time.h"

namespace ratcast {

/// The most a picture of `width` x `height` at `frame_rate` is worth:
/// `at_1080p60` for 1920 x 1080 at 60 frames a second, any other in
/// proportion to its pixels a second to the power 0.75 — a picture of four
/// times the pixels needs more bits, not four times as many (1080 lines at
/// 8 Mbit/s: 720 at 4.4, 1440 at 12.3, 2160 at 22.6) — and no less than
/// 1 Mbit/s.
uint32_t bitrate_for_size(uint32_t width, uint32_t height, uint32_t frame_rate,
                          uint32_t at_1080p60) noexcept;

struct SizeAdapterConfig {
    /// The most it goes up to; 0: as the pictures come.
    uint32_t max_lines = 0;
    uint32_t frame_rate = 60;
    /// What a size needs, as bitrate_for_size() says.
    uint32_t at_1080p60 = 8'000'000;
};

class SizeAdapter {
public:
    /// Sizes the standard steps go through, the most first.
    static constexpr uint32_t kLevels[] = {2160, 1440, 1080, 720};

    /// Starting at the most at `now`.
    SizeAdapter(SizeAdapterConfig config, Timestamp now);

    /// What the encoder takes at `now`: pictures of `width` x `height`, made
    /// no larger than lines() already; the bitrate rate control aims at; and
    /// how many pictures the source has given and how many of those the
    /// encoder skipped, so far. The limit from now on: lines(). It is never
    /// more than what the pictures have while they have less: nothing to
    /// gain.
    uint32_t update(Timestamp now, uint32_t width, uint32_t height, uint32_t bitrate,
                    uint64_t pictures, uint64_t skipped);

    /// The pictures' limit in lines: 0, none.
    uint32_t lines() const noexcept { return lines_; }

private:
    /// What `lines` lines of pictures shaped `aspect` (the longer side over
    /// the shorter) need.
    uint32_t needs(uint32_t lines, double aspect) const noexcept;
    /// The next size up from lines_, if there is one to go to: 0 for none.
    std::optional<uint32_t> next_up() const noexcept;
    void change(uint32_t lines, Timestamp now, uint64_t pictures, uint64_t skipped);

    const SizeAdapterConfig config_;
    uint32_t lines_;
    Timestamp changed_at_;
    std::optional<Timestamp> low_since_;   ///< the bitrate below what the size needs
    std::optional<Timestamp> high_since_;  ///< the bitrate enough for the next size up
    std::optional<Timestamp> up_at_;       ///< the last step up, until it has held
    Duration up_after_;                    ///< how long high_since_ must hold
    uint32_t ceiling_ = 0;                 ///< where the encoder fell behind; 0: none
    // What the encoder was given, counted from:
    Timestamp counted_since_;
    uint64_t pictures_ = 0;
    uint64_t skipped_ = 0;
};

}  // namespace ratcast
