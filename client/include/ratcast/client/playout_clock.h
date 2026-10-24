#pragma once

/**
 * @file playout_clock.h
 * @brief One sender's sound and pictures played in step: lip sync.
 *
 * A sender stamps its sound and its pictures on one clock, so what was
 * captured together carries the same capture time. Each player has its own
 * offset from capture to playing — the sound its jitter buffer's delay and the
 * device's, the pictures their playout schedule's delay and the decoder's —
 * and the two drift apart as the path changes. The PlayoutClock, shared by a
 * sender's audio and video players, brings them to one offset, the larger:
 *
 *   - The sound sets the pace: it cannot be held up without being heard, so
 *     the pictures are held until the sound captured with them is heard
 *     (due()).
 *   - Where the pictures need longer — a slow decoder, frames sent again on
 *     a lossy path — the sound is to wait for them: sound_floor() is the
 *     least delay its jitter buffer should keep, which it reaches by making up
 *     a frame now and then (JitterBuffer::set_min_delay()).
 *
 * Neither need is computed from the other's result, so the two do not chase
 * each other. A side that has said nothing for a second (the sound stopped,
 * the pictures stand still) no longer counts.
 *
 * Played in step with the sender's other viewers (play_together(),
 * common_delay.h), both go to the offset they all play at — the sound's floor
 * raised to it, the pictures held to it where there is no sound to follow.
 * What they all play at is made of what each needs for itself (need()): the
 * sound's jitter buffer's own aim and the pictures' own, never what either
 * plays at, which the others' needs raised.
 *
 * It also measures what it is for: how far apart, as they are played, are the
 * pictures from the sound captured with them — also when it is told not to
 * keep them in step, which is how to see what it does. Thread-safe.
 */

#include <cstdint>
#include <mutex>
#include <optional>

#include "ratcast/client/playback_meter.h"
#include "ratcast/core/time.h"
#include "ratcast/session/media_session.h"

namespace ratcast {

struct PlayoutClockStats {
    std::optional<Duration> sound;     ///< capture to heard, now
    std::optional<Duration> pictures;  ///< what the pictures need, capture to shown
    Duration floor{};                  ///< asked of the sound's jitter buffer
    std::optional<Duration> need;      ///< what this side needs, capture to played
    /// What the sender's viewers all play at, from capture on its clock.
    std::optional<Duration> together;
    /// For each picture shown while there was sound: how far from the sound
    /// captured with it, either way.
    LatencyHistogram apart;
    /// Of those, the mean with its sign: positive when the pictures were late.
    Duration mean_apart{};
};

class PlayoutClock {
public:
    /// `clock` is the players': what play times and arrivals are on. Not
    /// `in_step`: each side keeps its own time, and the clock only measures.
    explicit PlayoutClock(Clock& clock = SteadyClock::instance(), bool in_step = true) noexcept
        : clock_(clock), in_step_(in_step) {}

    // ── The audio player's side ─────────────────────────────────────────────

    /// Sound captured at `capture` is heard at `play_time`, of which
    /// `buffered` is the jitter buffer's delay: what the floor can change.
    /// `wanted` is what it would be without one (JitterBuffer::wanted()).
    void sound_played(Timestamp capture, Timestamp play_time, Duration buffered, Duration wanted);

    /// The least delay for the sound's jitter buffer, so that the sound is not
    /// ahead of the pictures, nor of the others playing in step; zero when
    /// they need no longer than it.
    Duration sound_floor() const;

    // ── The video player's side ─────────────────────────────────────────────

    /// The pictures need `offset` from capture to being shown: their playout
    /// schedule's delay past the transit, and decoding.
    void pictures_need(Duration offset);

    /// When to show a picture captured at `capture` that the pictures' own
    /// schedule would show at `own_due`: as the sound captured with it is
    /// heard, if that is later, but never more than `max_hold` later.
    Timestamp due(Timestamp capture, Timestamp own_due, Duration max_hold) const;

    /// A picture captured at `capture` is shown now.
    void picture_shown(Timestamp capture);

    // ── In step with the sender's other viewers ─────────────────────────────

    /// What this side needs from capture to played, sound and pictures, for
    /// itself; none while neither said anything for a second.
    std::optional<Duration> need() const;

    /// Play at what the sender's viewers all play at, from now on; none: in
    /// this side's own time.
    void play_together(std::optional<CommonPlayout> common);

    /// Another sender from now on: what the last one's sound and pictures
    /// needed no longer counts, as if each had been quiet for a second, nor
    /// what its viewers played at. What was measured stays.
    void forget();

    PlayoutClockStats stats() const;

private:
    struct Sound {
        Duration heard{};   ///< capture to heard
        Duration rest{};    ///< of which not the jitter buffer's
        Duration wanted{};  ///< what the jitter buffer would keep with no floor
        Timestamp at{};     ///< when said
    };
    struct Pictures {
        Duration need{};
        Timestamp at{};
    };
    std::optional<Sound> sound(Timestamp now) const noexcept;
    std::optional<Duration> pictures(Timestamp now) const noexcept;
    Duration floor(const std::optional<Sound>& heard,
                   std::optional<Duration> pictures) const noexcept;
    static std::optional<Duration> need(const std::optional<Sound>& heard,
                                        std::optional<Duration> pictures) noexcept;

    Clock& clock_;
    const bool in_step_;
    mutable std::mutex mutex_;
    std::optional<Sound> sound_;
    std::optional<Pictures> pictures_;
    std::optional<CommonPlayout> together_;
    LatencyHistogram apart_;
    int64_t apart_sum_us_ = 0;  ///< signed
};

}  // namespace ratcast
