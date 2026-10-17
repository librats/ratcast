#include "ratcast/session/rate_controller.h"

#include <algorithm>
#include <cmath>

namespace ratcast {
namespace {

/// A cut leaves this much of what the path carries, and at least this much of
/// the target: a path that seems to carry far less is better found out in a
/// few cuts than believed at once.
constexpr double kBackoff = 0.85;
constexpr double kMaxCut = 0.5;
/// An estimate that grew more than this since the last sample is still being
/// found (as BBR's Startup finds it), not a ceiling.
constexpr double kStillGrowing = 1.25;
/// Of what a path is known to carry, the most a raise aims at.
constexpr double kHeadroom = 0.9;
/// A step up with no ceiling to approach, and one towards a ceiling.
constexpr double kRampGain = 1.5;
constexpr double kProbeGain = 1.08;
/// Past the ceiling by this much with no trouble, the path has grown.
constexpr double kPastCeiling = 1.15;
/// A raise smaller than this is not worth a keyframe.
constexpr double kMinRaise = 1.05;
constexpr Duration kRaiseInterval = std::chrono::seconds(1);
constexpr Duration kHoldAfterCut = std::chrono::seconds(2);

}  // namespace

bool is_valid(const BitrateLimits& limits) noexcept {
    return limits.min > 0 && limits.min <= limits.start && limits.start <= limits.max;
}

// ── PathMonitor ─────────────────────────────────────────────────────────────

PathSignal PathMonitor::update(const PathStats& sample, Timestamp now) {
    PathSignal signal;
    if (!sample.valid) return signal;
    signal.rtt = Duration{sample.srtt_us};

    const Duration queue_delay =
        sample.bandwidth_bps > 0
            ? Duration{static_cast<Duration::rep>(double(sample.queued_bytes) * 8e6 /
                                                  double(sample.bandwidth_bps))}
            : Duration::zero();
    signal.queue_delay = std::min(queue_delay, last_queue_delay_);
    last_queue_delay_ = queue_delay;

    // The counters start at zero with the connection, so the first sample
    // counts from there; a rate needs the time between two.
    signal.expired = sample.live_expired > (last_ ? last_->live_expired : 0);
    double repair_bps = 0;
    if (last_ && now > last_at_) {
        const double seconds = std::chrono::duration<double>(now - last_at_).count();
        repair_bps = double(sample.live_repair_bytes - last_->live_repair_bytes) * 8 / seconds;
    }
    // A keyframe alone keeps a path busy for a moment; one that stays busy
    // with an estimate that has stopped growing is full.
    const bool full = last_ && !last_->app_limited && !sample.app_limited &&
                      sample.bandwidth_bps > 0 &&
                      double(sample.bandwidth_bps) <= double(last_->bandwidth_bps) * kStillGrowing;
    if (full) {
        signal.capacity =
            static_cast<uint64_t>(std::max(double(sample.bandwidth_bps) - repair_bps, 0.0));
    }

    last_ = sample;
    last_at_ = now;
    return signal;
}

// ── RateController ──────────────────────────────────────────────────────────

RateController::RateController(BitrateLimits limits, Duration lifetime) noexcept
    : limits_(limits), lifetime_(lifetime), target_(limits.start) {}

bool RateController::overused(const PathSignal& signal) const noexcept {
    return signal.expired || signal.queue_delay > lifetime_ / 4;
}

std::optional<uint32_t> RateController::on_overuse(Timestamp now, Duration rtt,
                                                   std::optional<uint64_t> capacity) noexcept {
    // Frames sent before the last cut are still on their way, and may yet
    // expire; they say nothing about the new rate.
    if (last_cut_ && now - *last_cut_ < lifetime_ + rtt) return std::nullopt;
    last_cut_ = now;
    double from = target_;
    if (capacity) {
        from = std::clamp(double(*capacity), target_* kMaxCut, from);
        ceiling_ = from;
    }
    return set_target(from * kBackoff);
}

std::optional<uint32_t> RateController::on_clear(Timestamp now,
                                                 std::optional<uint64_t> capacity) noexcept {
    if (!last_raise_) {
        // The first word from a path: the start has yet to prove itself.
        last_raise_ = now;
        return std::nullopt;
    }
    if (now - *last_raise_ < kRaiseInterval) return std::nullopt;
    if (last_cut_ && now - *last_cut_ < kHoldAfterCut) return std::nullopt;

    if (ceiling_ && target_ > *ceiling_ * kPastCeiling) ceiling_.reset();
    double next = target_ * kRampGain;
    if (ceiling_) {
        // Quickly back to where a cut at the ceiling leaves it, carefully on.
        const double below = *ceiling_ * kBackoff;
        next = target_ < below ? std::min(next, below) : target_ * kProbeGain;
    }
    if (capacity) next = std::min(next, double(*capacity) * kHeadroom);
    if (next < target_ * kMinRaise) return std::nullopt;
    last_raise_ = now;
    return set_target(next);
}

std::optional<uint32_t> RateController::restart() noexcept {
    ceiling_.reset();
    last_cut_.reset();
    last_raise_.reset();
    return set_target(limits_.start);
}

std::optional<uint32_t> RateController::set_target(double bits_per_second) noexcept {
    const auto next = static_cast<uint32_t>(
        std::clamp(std::round(bits_per_second), double(limits_.min), double(limits_.max)));
    if (next == target_) return std::nullopt;
    target_ = next;
    return next;
}

}  // namespace ratcast
