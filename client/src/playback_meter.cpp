#include "ratcast/client/playback_meter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace ratcast {

// ── LatencyHistogram ────────────────────────────────────────────────────────
//
// Below kSub µs every value has a bucket of its own. Above, a value with its
// top bit at position e keeps its top kSubBits + 1 bits (the "mantissa", kSub
// to 2 kSub - 1) and the number of bits shifted away; bucket = shift * kSub +
// mantissa continues the exact range without a seam.

size_t LatencyHistogram::bucket(uint64_t us) noexcept {
    if (us < kSub) return static_cast<size_t>(us);
    const int shift = std::bit_width(us) - 1 - kSubBits;
    const size_t index = static_cast<size_t>(shift) * kSub + static_cast<size_t>(us >> shift);
    return std::min<size_t>(index, kBuckets - 1);
}

uint64_t LatencyHistogram::upper_bound(size_t bucket) noexcept {
    if (bucket < 2 * kSub) return bucket;  // one value each
    const int shift = static_cast<int>(bucket / kSub) - 1;
    const uint64_t mantissa = bucket % kSub + kSub;
    return ((mantissa + 1) << shift) - 1;
}

void LatencyHistogram::add(Duration value) noexcept {
    const uint64_t us = static_cast<uint64_t>(std::max<Duration::rep>(value.count(), 0));
    ++buckets_[bucket(us)];
    ++count_;
    sum_us_ += us;
    max_ = std::max(max_, Duration{static_cast<Duration::rep>(us)});
}

void LatencyHistogram::clear() noexcept { *this = LatencyHistogram{}; }

Duration LatencyHistogram::mean() const noexcept {
    return count_ == 0 ? Duration{} : Duration{static_cast<Duration::rep>(sum_us_ / count_)};
}

Duration LatencyHistogram::percentile(double p) const noexcept {
    if (count_ == 0) return Duration{};
    const auto rank = static_cast<uint64_t>(
        std::clamp(std::ceil(p * static_cast<double>(count_)), 1.0, static_cast<double>(count_)));
    uint64_t seen = 0;
    for (size_t b = 0; b < buckets_.size(); ++b) {
        seen += buckets_[b];
        if (seen >= rank) {
            // The bucket's top, but never past the largest value recorded; the
            // last bucket also holds everything beyond it.
            if (b == buckets_.size() - 1) return max_;
            return std::min(Duration{static_cast<Duration::rep>(upper_bound(b))}, max_);
        }
    }
    return max_;
}

LatencyHistogram LatencyHistogram::since(const LatencyHistogram& earlier) const noexcept {
    if (earlier.count_ > count_) return *this;  // started over
    LatencyHistogram added;
    for (size_t b = 0; b < buckets_.size(); ++b)
        added.buckets_[b] = buckets_[b] - earlier.buckets_[b];
    added.count_ = count_ - earlier.count_;
    added.sum_us_ = sum_us_ - earlier.sum_us_;
    if (max_ > earlier.max_) {
        added.max_ = max_;
        return added;
    }
    // The top of the highest bucket added to, never past this one's maximum.
    for (size_t b = buckets_.size(); b-- > 0;) {
        if (added.buckets_[b] == 0) continue;
        added.max_ = b == buckets_.size() - 1
                         ? max_
                         : std::min(Duration{static_cast<Duration::rep>(upper_bound(b))}, max_);
        break;
    }
    return added;
}

// ── FramePace ───────────────────────────────────────────────────────────────

void FramePace::picture(Timestamp now) noexcept {
    if (last_) {
        const Duration interval = now - *last_;
        intervals_.add(interval);
        if (last_interval_) {
            changes_ += interval > *last_interval_ ? interval - *last_interval_
                                                   : *last_interval_ - interval;
            ++changed_;
        }
        last_interval_ = interval;
    }
    last_ = now;
}

void FramePace::restart() noexcept {
    last_.reset();
    last_interval_.reset();
}

Duration FramePace::jitter() const noexcept {
    return changed_ == 0 ? Duration{} : changes_ / static_cast<Duration::rep>(changed_);
}

FramePace FramePace::since(const FramePace& earlier) const noexcept {
    FramePace added;
    added.intervals_ = intervals_.since(earlier.intervals_);
    const bool started_over = earlier.changed_ > changed_;
    added.changes_ = started_over ? changes_ : changes_ - earlier.changes_;
    added.changed_ = started_over ? changed_ : changed_ - earlier.changed_;
    return added;
}

// ── PlaybackMeter ───────────────────────────────────────────────────────────

PlaybackMeter::PlaybackMeter(Clock& clock, Duration stall_after)
    : clock_(clock), stall_after_(stall_after), started_(clock.now()) {}

void PlaybackMeter::start() {
    std::lock_guard lock(mutex_);
    started_ = clock_.now();
    last_picture_.reset();
    total_ = PlaybackStats{};
    interval_ = PlaybackStats{};
}

void PlaybackMeter::picture(std::optional<Duration> latency) {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    if (!total_.first_picture) total_.first_picture = now - started_;
    for (PlaybackStats* stats : {&total_, &interval_}) {
        ++stats->pictures;
        if (latency) stats->latency.add(*latency);
        if (!last_picture_) continue;
        const Duration gap = now - *last_picture_;
        stats->longest_gap = std::max(stats->longest_gap, gap);
        if (gap > stall_after_) {
            ++stats->stalls;
            stats->stalled += gap;
        }
    }
    last_picture_ = now;
}

PlaybackStats PlaybackMeter::total() const {
    std::lock_guard lock(mutex_);
    return total_;
}

PlaybackStats PlaybackMeter::take_interval() {
    std::lock_guard lock(mutex_);
    return std::exchange(interval_, PlaybackStats{});
}

}  // namespace ratcast
