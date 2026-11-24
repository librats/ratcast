#include "ratcast/client/size_adapter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace ratcast {
namespace {

using namespace std::chrono_literals;

/// After a change, nothing again for this long: the encoder starts over and
/// rate control finds its feet.
constexpr Duration kSettle = 5s;
/// How long the bitrate stays short of what the size needs before it goes.
constexpr Duration kLowFor = 3s;
/// What the encoder skipped is counted over this long.
constexpr Duration kCountOver = 2s;
/// What a size needs, of bitrate_for_size()'s, to be kept, and to be gone up to.
constexpr double kKeep = 0.35;
constexpr double kUp = 0.6;
/// How long the bitrate is enough for the next size up before it goes up,
/// at first and at most.
constexpr Duration kUpAfter = 10s;
constexpr Duration kUpAfterMost = 160s;
/// A step down this soon after a step up: the step up went too far.
constexpr Duration kCameBack = 20s;
/// A step up held this long has held.
constexpr Duration kHeld = 60s;
/// What a picture took the encoder where it fell behind, of the time between
/// pictures as they come now, to go back there: a film at 30 a second where
/// 4K took 19 ms (0.56) is in reach, a page scrolled at 60 (1.1) is not. Too
/// little for the film's content after all, it falls behind again: a step up
/// that came straight back.
constexpr double kKeepsUp = 0.7;
/// How long what a picture took the encoder where it fell behind is held to:
/// it may have been a moment of the GPU busy with something else, and nothing
/// measures it again at a size not sent. Tried again after it, and twice as
/// long after each try that fell behind again, up to the most.
constexpr Duration kBehindHeld = 5min;
constexpr Duration kBehindHeldMost = 40min;
/// Pictures coming at this much of the frame rate or more: something moves
/// to judge the encoder by, behind or keeping up. A screen still for a while
/// says nothing of the next scroll, and a picture or two of it skipped
/// nothing of the encoder.
constexpr double kMoving = 0.25;
/// A level below is a step only at this much of the lines there are or
/// fewer: 1920 x 1200 goes to 1080, 1366 x 768 not to 720.
constexpr double kStep = 0.9;

double seconds(Duration duration) noexcept {
    return std::chrono::duration<double>(duration).count();
}

}  // namespace

uint32_t bitrate_for_size(uint32_t width, uint32_t height, uint32_t frame_rate,
                          uint32_t at_1080p60) noexcept {
    const double rate = double(width) * height * frame_rate / (1920.0 * 1080 * 60);
    const double bitrate = at_1080p60 * std::pow(rate, 0.75);
    return uint32_t(std::clamp(bitrate, 1'000'000.0, double(std::numeric_limits<uint32_t>::max())));
}

SizeAdapter::SizeAdapter(SizeAdapterConfig config, Timestamp now)
    : config_(config),
      lines_(config.max_lines),
      changed_at_(now),
      up_after_(kUpAfter),
      counted_since_(now) {}

uint32_t SizeAdapter::needs(uint32_t lines, double aspect) const noexcept {
    return bitrate_for_size(uint32_t(lines * aspect), lines, config_.frame_rate,
                            config_.at_1080p60);
}

std::optional<uint32_t> SizeAdapter::next_up() const noexcept {
    std::optional<uint32_t> next;
    for (const uint32_t level : kLevels) {
        if (level > lines_ && (config_.max_lines == 0 || level <= config_.max_lines)) {
            next = level;  // the least above
        }
    }
    if (next) return next;
    if (config_.max_lines > lines_) return config_.max_lines;
    if (config_.max_lines == 0) return 0u;  // as they come
    return std::nullopt;
}

bool SizeAdapter::keeps_up(uint32_t lines, Timestamp now) const noexcept {
    for (const Behind& behind : behind_) {
        if (lines != 0 && behind.lines > lines) continue;  // larger pictures than these
        // Still: nothing a try would be judged by, held to or not — tried on
        // a page being read, it would come back at the next scroll and be
        // counted as having kept up.
        if (!rate_ || *rate_ < config_.frame_rate * kMoving) return false;
        if (now - behind.at >= behind.held) continue;  // to be tried again
        if (seconds(behind.took) * *rate_ > kKeepsUp) return false;
    }
    return true;
}

void SizeAdapter::change(uint32_t lines, Timestamp now, const EncoderCounts& counts) {
    lines_ = lines;
    changed_at_ = now;
    low_since_.reset();
    high_since_.reset();
    counted_since_ = now;
    counted_ = counts;
}

uint32_t SizeAdapter::update(Timestamp now, uint32_t width, uint32_t height, uint32_t bitrate,
                             const EncoderCounts& counts) {
    const uint32_t shorter = std::min(width, height);
    if (shorter == 0) return lines_;
    if (now - changed_at_ < kSettle) {
        counted_since_ = now;
        counted_ = counts;
        return lines_;
    }
    const double aspect = double(std::max(width, height)) / shorter;
    if (up_at_ && now - *up_at_ >= kHeld) {
        up_at_.reset();
        up_after_ = kUpAfter;
    }
    const auto step_down = [&](uint32_t lines) {
        if (up_at_ && now - *up_at_ < kCameBack) up_after_ = std::min(up_after_ * 2, kUpAfterMost);
        up_at_.reset();
        change(lines, now, counts);
        return lines_;
    };
    const auto below = [&](uint32_t level) { return level <= shorter * kStep; };

    // The encoder behind: a level down, and what a picture took it kept for
    // pictures of these lines. Not while it has just started over: counted
    // from when it has found its feet.
    if (started_at_ && now - *started_at_ < kSettle) {
        counted_since_ = now;
        counted_ = counts;
    } else if (now - counted_since_ >= kCountOver) {
        const uint64_t given = counts.pictures - counted_.pictures;
        const uint64_t missed = counts.skipped - counted_.skipped;
        const uint64_t taken = counts.taken - counted_.taken;
        const Duration took = counts.busy - counted_.busy;
        const Duration over = now - counted_since_;
        rate_ = double(given) / seconds(over);
        counted_since_ = now;
        counted_ = counts;
        if (*rate_ >= config_.frame_rate * kMoving && missed * 5 > given) {
            for (const uint32_t level : kLevels) {
                if (!below(level)) continue;
                // None taken on: one picture still at it all the count long,
                // what it took counted only once it is done.
                const Duration each = taken > 0 ? took / int64_t(taken) : over;
                const auto it = std::ranges::find(behind_, shorter, &Behind::lines);
                if (it == behind_.end()) {
                    behind_.push_back({shorter, each, now, kBehindHeld});
                } else {
                    // Tried again for being held to no longer, and behind
                    // again before the step up held: held to twice as long.
                    // Behind after a while kept up: as at first.
                    if (now - it->at >= it->held) {
                        it->held = up_at_ ? std::min(it->held * 2, kBehindHeldMost) : kBehindHeld;
                    }
                    it->took = each;
                    it->at = now;
                }
                return step_down(level);
            }
        }
    }

    // The path narrower than the size needs: the largest it keeps, or the least.
    if (bitrate < needs(shorter, aspect) * kKeep) {
        if (!low_since_) low_since_ = now;
    } else {
        low_since_.reset();
    }
    if (low_since_ && now - *low_since_ >= kLowFor) {
        std::optional<uint32_t> to;
        for (const uint32_t level : kLevels) {
            if (!below(level)) continue;
            to = level;
            if (bitrate >= needs(level, aspect) * kKeep) break;
        }
        if (to) return step_down(*to);
    }

    // Room on the path for the next size up, held long enough.
    const std::optional<uint32_t> next =
        lines_ != 0 && shorter >= lines_ ? next_up() : std::nullopt;
    if (!next) {
        high_since_.reset();
        return lines_;
    }
    // As they come: a screen of more lines than the most level is not known
    // to need more than that level.
    const uint32_t next_lines = *next != 0 ? *next : lines_;
    if (bitrate >= needs(next_lines, aspect) * kUp && keeps_up(*next, now)) {
        if (!high_since_) high_since_ = now;
    } else {
        high_since_.reset();
    }
    if (high_since_ && now - *high_since_ >= up_after_) {
        change(*next, now, counts);
        up_at_ = now;
    }
    return lines_;
}

}  // namespace ratcast
