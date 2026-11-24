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
 *     over two seconds, while they came at a quarter of the frame rate or
 *     more: a film at 24 is judged as a page scrolled at 60, a screen still
 *     but for a picture or two is not. Not counted for five seconds after the
 *     encoder started over (encoder_started()): what came while it opened, as
 *     the window shared changed size, was skipped however fast it is. What a
 *     picture took it there (made to fit and encoded: EncoderCounts::busy) is
 *     kept for pictures of that many lines, and as
 *     many or more are gone back up to only while the pictures come far
 *     enough apart for that to be 70% of the time between them or less, and
 *     still come at a quarter of the frame rate or more (a screen still for a
 *     while says nothing of the next scroll): a page
 *     scrolled at 60 a second may be too much for 4K where a film at 24 is
 *     not — what keeps up depends on the content and its rate, not on the
 *     computer alone. Falling behind again soon after counts as a step up
 *     that came straight back, below. What a picture took is held to for
 *     five minutes (a moment of the GPU busy with a game says nothing of the
 *     rest of the session), then tried again: twice as long after each try
 *     that fell behind again within a minute, up to forty minutes.
 *   - Up a level once the bitrate has stayed at 60% of what it needs (and the
 *     encoder kept up with that size, as above) for ten seconds: twice as
 *     long after each step up that came straight back down (within twenty
 *     seconds), up to 160, back to ten after a minute held.
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
#include <vector>

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

/// What the encoder of a share has been given and done so far: counts that
/// only grow.
struct EncoderCounts {
    uint64_t pictures = 0;  ///< from the source
    uint64_t skipped = 0;   ///< of those, replaced by a newer one before it took them
    uint64_t taken = 0;     ///< pictures it took on, a still screen's repeated among them
    Duration busy{};        ///< spent on them: each made to fit, then encoded
};

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
    /// what the encoder has been given and done so far. The limit from now
    /// on: lines(). It is never more than what the pictures have while they
    /// have less: nothing to gain.
    uint32_t update(Timestamp now, uint32_t width, uint32_t height, uint32_t bitrate,
                    const EncoderCounts& counts);

    /// The encoder started over at `now` — opened anew, for pictures of
    /// another size or kind: what it skips is not counted for a while.
    void encoder_started(Timestamp now) noexcept { started_at_ = now; }

    /// The pictures' limit in lines: 0, none.
    uint32_t lines() const noexcept { return lines_; }

private:
    /// What `lines` lines of pictures shaped `aspect` (the longer side over
    /// the shorter) need.
    uint32_t needs(uint32_t lines, double aspect) const noexcept;
    /// The next size up from lines_, if there is one to go to: 0 for as
    /// they come.
    std::optional<uint32_t> next_up() const noexcept;
    /// Whether the encoder keeps up with pictures of `lines` lines (0: as
    /// they come) at the rate they come now, by what one took it where it
    /// fell behind at as many lines or fewer, still held to at `now`.
    bool keeps_up(uint32_t lines, Timestamp now) const noexcept;
    void change(uint32_t lines, Timestamp now, const EncoderCounts& counts);

    const SizeAdapterConfig config_;
    uint32_t lines_;
    Timestamp changed_at_;
    std::optional<Timestamp> low_since_;   ///< the bitrate below what the size needs
    std::optional<Timestamp> high_since_;  ///< the bitrate enough for the next size up
    std::optional<Timestamp> up_at_;       ///< the last step up, until it has held
    Duration up_after_;                    ///< how long high_since_ must hold
    /// Where the encoder fell behind: the pictures' lines then, what one
    /// took it, when, and how long that is held to from then.
    struct Behind {
        uint32_t lines;
        Duration took;
        Timestamp at;
        Duration held;
    };
    std::vector<Behind> behind_;
    /// What the encoder was given, counted from:
    Timestamp counted_since_;
    EncoderCounts counted_;
    /// Pictures a second the source gave over the last count; none yet.
    std::optional<double> rate_;
    std::optional<Timestamp> started_at_;  ///< the encoder's last start over
};

}  // namespace ratcast
