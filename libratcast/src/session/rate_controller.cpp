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
/// Pacing faster than this many times the estimate is a congestion controller
/// still finding the path (BBR's Startup: 2.77).
constexpr double kStartupPacing = 2.0;
/// Of what a path is known to carry, the most a raise aims at.
constexpr double kHeadroom = 0.9;
/// A step up with no ceiling to approach, and one towards a ceiling.
constexpr double kRampGain = 1.5;
constexpr double kProbeGain = 1.08;
/// Carried past the ceiling by this much, the path has grown.
constexpr double kPastCeiling = 1.15;
/// A raise smaller than this is not worth a keyframe.
constexpr double kMinRaise = 1.05;
constexpr Duration kRaiseInterval = std::chrono::seconds(1);
/// The steps past a ceiling found again where it was are this far apart at
/// most.
constexpr Duration kProbeEveryMost = std::chrono::seconds(8);
/// What a path carried is measured over this much.
constexpr Duration kCarriedOver = std::chrono::seconds(1);
/// A round trip's rise is over the least of this much.
constexpr Duration kRiseOver = std::chrono::seconds(10);
constexpr Duration kHoldAfterCut = std::chrono::seconds(2);

}  // namespace

bool is_valid(const BitrateLimits& limits) noexcept {
    return limits.min > 0 && limits.min <= limits.start && limits.start <= limits.max;
}

PathSignal share_of(const PathSignal& path, double share) noexcept {
    const auto part = [share](std::optional<uint64_t> bits) -> std::optional<uint64_t> {
        if (!bits) return std::nullopt;
        return static_cast<uint64_t>(std::round(double(*bits) * share));
    };
    PathSignal shared = path;
    shared.capacity = part(path.capacity);
    shared.delivered = part(path.delivered);
    shared.carried = part(path.carried);
    return shared;
}

bool overuses(const PathSignal& signal, Duration lifetime) noexcept {
    return !signal.starting && (signal.expired || signal.queue_delay > lifetime / 4);
}

// ── PathMonitor ─────────────────────────────────────────────────────────────

PathSignal PathMonitor::update(const PathStats& sample, Timestamp now) {
    PathSignal signal;
    if (!sample.valid) return signal;
    signal.rtt = Duration{sample.srtt_us};
    while (!least_rtts_.empty() && least_rtts_.back().second >= signal.rtt) least_rtts_.pop_back();
    least_rtts_.emplace_back(now, signal.rtt);
    while (now - least_rtts_.front().first > kRiseOver) least_rtts_.pop_front();
    signal.queue_rise = signal.rtt - least_rtts_.front().second;
    signal.starting = double(sample.pacing_bps) > double(sample.bandwidth_bps) * kStartupPacing;

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
    const uint64_t media =
        sample.bytes_delivered - std::min(sample.live_repair_bytes, sample.bytes_delivered);
    if (!last_) media_ = {{now, media}};
    if (!last_ || now <= last_at_) {
        last_ = sample;
        last_at_ = now;
        return signal;
    }
    while (media_.size() > 1 && now - media_[1].first >= kCarriedOver) media_.pop_front();
    const auto& [since, before] = media_.front();
    signal.carried = static_cast<uint64_t>(double(media - std::min(before, media)) * 8 /
                                           std::chrono::duration<double>(now - since).count());
    media_.emplace_back(now, media);

    signal.since = now - last_at_;
    const double seconds = std::chrono::duration<double>(signal.since).count();
    const double repair_bps =
        double(sample.live_repair_bytes - last_->live_repair_bytes) * 8 / seconds;
    const double delivered_bps =
        double(sample.bytes_delivered - last_->bytes_delivered) * 8 / seconds;
    // A keyframe alone keeps a path busy for a moment; one busy in two samples
    // running delivered what it carries in between.
    if (!signal.starting && !last_->app_limited && !sample.app_limited) {
        signal.delivered = static_cast<uint64_t>(std::max(delivered_bps - repair_bps, 0.0));
        // With an estimate that has stopped growing, it is full.
        if (sample.bandwidth_bps > 0 &&
            double(sample.bandwidth_bps) <= double(last_->bandwidth_bps) * kStillGrowing) {
            signal.capacity =
                static_cast<uint64_t>(std::max(double(sample.bandwidth_bps) - repair_bps, 0.0));
        }
    }

    last_ = sample;
    last_at_ = now;
    return signal;
}

// ── RateController ──────────────────────────────────────────────────────────

RateController::RateController(BitrateLimits limits, Duration lifetime) noexcept
    : limits_(limits), lifetime_(lifetime), target_(limits.start), probe_every_(kRaiseInterval) {}

std::optional<uint32_t> RateController::on_overuse(Timestamp now, const PathSignal& path) noexcept {
    // Frames sent before the last cut are still on their way, and may yet
    // expire; they say nothing about the new rate.
    if (last_cut_ && now - *last_cut_ < lifetime_ + path.rtt) return std::nullopt;
    last_cut_ = now;
    // What the path carries: the lesser of what it delivered and its estimate,
    // which lags a path that narrowed.
    std::optional<uint64_t> carries = path.delivered;
    if (path.capacity) carries = std::min(carries.value_or(*path.capacity), *path.capacity);
    double from = target_;
    if (carries) {
        // Filled up again where it did before: probing past that finds
        // nothing new, only more cuts. Narrower: as new as any.
        probe_every_ = ceiling_ && double(*carries) >= *ceiling_ * kBackoff
                           ? std::min(probe_every_ * 2, kProbeEveryMost)
                           : kRaiseInterval;
        from = std::clamp(double(*carries), target_* kMaxCut, from);
        ceiling_ = from;
    }
    return set_target(from * kBackoff);
}

std::optional<uint32_t> RateController::on_clear(Timestamp now, std::optional<uint64_t> capacity,
                                                 std::optional<uint64_t> carried) noexcept {
    if (!last_raise_) {
        // The first word from a path: the start has yet to prove itself.
        last_raise_ = now;
        return std::nullopt;
    }
    if (now - *last_raise_ < kRaiseInterval) return std::nullopt;
    if (last_cut_ && now - *last_cut_ < kHoldAfterCut) return std::nullopt;

    if (ceiling_ && carried && *carried > *ceiling_ * kPastCeiling) {
        ceiling_.reset();  // the path has grown
        probe_every_ = kRaiseInterval;
    }
    double next = target_ * kRampGain;
    if (ceiling_) {
        // Quickly back to where a cut at the ceiling leaves it, carefully on
        // from there — and from short of it by less than a raise is worth.
        const double below = *ceiling_ * kBackoff;
        if (target_ * kMinRaise < below) {
            next = std::min(next, below);
        } else {
            const Timestamp stepped = std::max(*last_raise_, last_cut_.value_or(*last_raise_));
            if (now - stepped < probe_every_) return std::nullopt;
            next = target_ * kProbeGain;
        }
    }
    if (capacity) next = std::min(next, double(*capacity) * kHeadroom);
    if (next < target_ * kMinRaise) return std::nullopt;
    last_raise_ = now;
    return set_target(next);
}

std::optional<uint32_t> RateController::limit(uint64_t capacity) noexcept {
    const double most = double(capacity) * kHeadroom;
    if (target_ <= most) return std::nullopt;
    return set_target(most);
}

std::optional<uint32_t> RateController::restart() noexcept {
    ceiling_.reset();
    probe_every_ = kRaiseInterval;
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
