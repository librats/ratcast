#include "ratcast/client/playout_clock.h"

#include <algorithm>

namespace ratcast {
namespace {

/// A side that said nothing for this long no longer counts.
constexpr Duration kStale = std::chrono::seconds(1);

}  // namespace

void PlayoutClock::sound_played(Timestamp capture, Timestamp play_time, Duration buffered,
                                Duration wanted) {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    sound_ = Sound{play_time - capture, play_time - capture - buffered, wanted, now};
}

Duration PlayoutClock::sound_floor() const {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    return floor(sound(now), pictures(now));
}

// What the jitter buffer is to keep for the sound to be heard as late as the
// pictures need, and as the others play.
Duration PlayoutClock::floor(const std::optional<Sound>& heard,
                             std::optional<Duration> pictures) const noexcept {
    if (!heard) return Duration::zero();
    Duration aim{};
    if (in_step_ && pictures) aim = *pictures;
    if (together_) aim = std::max(aim, together_->offset);
    return std::max(aim - heard->rest, Duration::zero());
}

void PlayoutClock::pictures_need(Duration offset) {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    pictures_ = Pictures{offset, now};
}

Timestamp PlayoutClock::due(Timestamp capture, Timestamp own_due, Duration max_hold) const {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    // The sound captured with it, which goes to the others' time itself; or,
    // with no sound to follow, the others' time.
    const std::optional<Sound> heard = sound(now);
    Timestamp aim;
    if (in_step_ && heard) {
        aim = capture + heard->heard;
    } else if (together_) {
        aim = capture + together_->offset;
    } else {
        return own_due;
    }
    return std::min(std::max(own_due, aim), own_due + max_hold);
}

void PlayoutClock::picture_shown(Timestamp capture) {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    const std::optional<Sound> heard = sound(now);
    if (!heard) return;
    const Duration apart = (now - capture) - heard->heard;
    apart_.add(apart < Duration::zero() ? -apart : apart);
    apart_sum_us_ += apart.count();
}

std::optional<Duration> PlayoutClock::need() const {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    return need(sound(now), pictures(now));
}

// The more of what the pictures and the sound need for themselves.
std::optional<Duration> PlayoutClock::need(const std::optional<Sound>& heard,
                                           std::optional<Duration> pictures) noexcept {
    if (!heard) return pictures;
    const Duration sound = heard->rest + heard->wanted;
    return pictures ? std::max(*pictures, sound) : sound;
}

void PlayoutClock::play_together(std::optional<CommonPlayout> common) {
    std::lock_guard lock(mutex_);
    together_ = common;
}

void PlayoutClock::forget() {
    std::lock_guard lock(mutex_);
    sound_.reset();
    pictures_.reset();
    together_.reset();
}

PlayoutClockStats PlayoutClock::stats() const {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    PlayoutClockStats stats;
    const std::optional<Sound> heard = sound(now);
    if (heard) stats.sound = heard->heard;
    stats.pictures = pictures(now);
    stats.floor = floor(heard, stats.pictures);
    stats.need = need(heard, stats.pictures);
    if (together_) stats.together = together_->delay;
    stats.apart = apart_;
    if (apart_.count() > 0) {
        stats.mean_apart = Duration{apart_sum_us_ / static_cast<int64_t>(apart_.count())};
    }
    return stats;
}

std::optional<PlayoutClock::Sound> PlayoutClock::sound(Timestamp now) const noexcept {
    if (!sound_ || now - sound_->at > kStale) return std::nullopt;
    return sound_;
}

std::optional<Duration> PlayoutClock::pictures(Timestamp now) const noexcept {
    if (!pictures_ || now - pictures_->at > kStale) return std::nullopt;
    return pictures_->need;
}

}  // namespace ratcast
