#include "ratcast/client/playout_clock.h"

#include <algorithm>

namespace ratcast {
namespace {

/// A side that said nothing for this long no longer counts.
constexpr Duration kStale = std::chrono::seconds(1);

}  // namespace

void PlayoutClock::sound_played(Timestamp capture, Timestamp play_time, Duration buffered) {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    sound_ = Sound{play_time - capture, play_time - capture - buffered, now};
}

Duration PlayoutClock::sound_floor() const {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    const std::optional<Sound> heard = sound(now);
    const std::optional<Duration> need = pictures(now);
    if (!in_step_ || !heard || !need) return Duration::zero();
    return std::max(*need - heard->rest, Duration::zero());
}

void PlayoutClock::pictures_need(Duration offset) {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    pictures_ = Pictures{offset, now};
}

Timestamp PlayoutClock::due(Timestamp capture, Timestamp own_due, Duration max_hold) const {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    const std::optional<Sound> heard = sound(now);
    if (!in_step_ || !heard) return own_due;
    return std::min(std::max(own_due, capture + heard->heard), own_due + max_hold);
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

void PlayoutClock::forget() {
    std::lock_guard lock(mutex_);
    sound_.reset();
    pictures_.reset();
}

PlayoutClockStats PlayoutClock::stats() const {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    PlayoutClockStats stats;
    if (const std::optional<Sound> heard = sound(now)) stats.sound = heard->heard;
    stats.pictures = pictures(now);
    if (in_step_ && stats.sound && stats.pictures) {
        stats.floor = std::max(*stats.pictures - sound_->rest, Duration::zero());
    }
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
