#include "ratcast/client/playout_schedule.h"

#include <algorithm>

namespace ratcast {
namespace {

using namespace std::chrono_literals;

/// The fastest frame is looked for over this long, and one window back: long
/// enough to have seen frames on an idle path, short enough to follow a path
/// whose delay rose for good.
constexpr Duration kFastestWindow = 10s;
/// A frame this close to the fastest is on time: it ends a run of late ones.
constexpr Duration kOnTime = 5ms;
/// A run this close to a keyframe is the keyframe's: before it, an encoder
/// for a new bitrate opening alongside slowed the one at work; after it, the
/// frames waited behind it, or the new encoder was slow with its first few.
constexpr Duration kNearKeyframe = 200ms;
/// Late runs count this long after they end.
constexpr Duration kRunWindow = 10s;
/// How fast the delay shrinks, per time passing.
constexpr double kShrink = 0.05;

}  // namespace

void PlayoutSchedule::set_max_delay(Duration max_delay) noexcept {
    max_delay_ = max_delay;
    delay_ = std::min(delay_, max_delay_);
}

void PlayoutSchedule::on_frame(Timestamp capture, Timestamp arrival, bool keyframe) {
    const Duration transit = arrival - capture;
    if (!fastest_) {
        fastest_ = Fastest{transit, arrival, std::nullopt};
    } else if (arrival - fastest_->since >= kFastestWindow) {
        fastest_ = Fastest{transit, arrival, fastest_->current};
    } else {
        fastest_->current = std::min(fastest_->current, transit);
    }
    const Duration late = transit - fastest();
    if (keyframe) {
        last_keyframe_ = arrival;
        while (!runs_.empty() && arrival - runs_.back().at <= kNearKeyframe) runs_.pop_back();
        if (run_) run_->keyframe = true;
    }

    if (late > kOnTime) {
        if (!run_) {
            run_ = Run{};
            run_->keyframe = last_keyframe_ && arrival - *last_keyframe_ <= kNearKeyframe;
        }
        run_->late = std::max(run_->late, late);
        run_->at = arrival;
    } else if (run_) {
        if (!run_->keyframe && run_->late <= max_delay_) runs_.push_back(*run_);
        run_.reset();
    }
    while (!runs_.empty() && arrival - runs_.front().at > kRunWindow) runs_.pop_front();

    const Duration wanted = this->wanted();
    if (wanted > delay_) {
        delay_ = wanted;
    } else if (wanted < delay_) {
        const auto shrink =
            std::chrono::duration_cast<Duration>((arrival - last_arrival_) * kShrink);
        delay_ = std::max(delay_ - shrink, wanted);
    }
    last_arrival_ = arrival;
}

Duration PlayoutSchedule::wanted() const noexcept {
    Duration latest{};
    Duration second{};
    const auto count = [&](Duration late) {
        if (late > latest) {
            second = latest;
            latest = late;
        } else {
            second = std::max(second, late);
        }
    };
    for (const Run& run : runs_) count(run.late);
    // The run under way counts as far as it got, so that the delay covers the
    // rest of it — unless it is one that will not count.
    if (run_ && !run_->keyframe && run_->late <= max_delay_) count(run_->late);
    return std::min(second, max_delay_);
}

Timestamp PlayoutSchedule::due(Timestamp capture) const noexcept {
    if (!fastest_) return Timestamp{};  // nothing to go by: at once
    return capture + fastest() + delay_;
}

Duration PlayoutSchedule::fastest() const noexcept {
    return std::min(fastest_->current, fastest_->previous.value_or(fastest_->current));
}

void PlayoutSchedule::reset() noexcept {
    fastest_.reset();
    runs_.clear();
    run_.reset();
    last_keyframe_.reset();
    delay_ = Duration::zero();
}

}  // namespace ratcast
