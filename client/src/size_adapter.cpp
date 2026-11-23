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
/// A level below is a step only at this much of the lines there are or
/// fewer: 1920 x 1200 goes to 1080, 1366 x 768 not to 720.
constexpr double kStep = 0.9;

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
    const auto allowed = [&](uint32_t lines) {
        return (config_.max_lines == 0 || lines <= config_.max_lines) &&
               (ceiling_ == 0 || lines <= ceiling_);
    };
    std::optional<uint32_t> next;
    for (const uint32_t level : kLevels) {
        if (level > lines_ && allowed(level)) next = level;  // the least above
    }
    if (next) return next;
    if (config_.max_lines > lines_ && allowed(config_.max_lines)) return config_.max_lines;
    if (config_.max_lines == 0 && ceiling_ == 0) return 0u;  // as they come
    return std::nullopt;
}

void SizeAdapter::change(uint32_t lines, Timestamp now, uint64_t pictures, uint64_t skipped) {
    lines_ = lines;
    changed_at_ = now;
    low_since_.reset();
    high_since_.reset();
    counted_since_ = now;
    pictures_ = pictures;
    skipped_ = skipped;
}

uint32_t SizeAdapter::update(Timestamp now, uint32_t width, uint32_t height, uint32_t bitrate,
                             uint64_t pictures, uint64_t skipped) {
    const uint32_t shorter = std::min(width, height);
    if (shorter == 0) return lines_;
    if (now - changed_at_ < kSettle) {
        counted_since_ = now;
        pictures_ = pictures;
        skipped_ = skipped;
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
        change(lines, now, pictures, skipped);
        return lines_;
    };
    const auto below = [&](uint32_t level) { return level <= shorter * kStep; };

    // The encoder behind: a level down, for good.
    if (now - counted_since_ >= kCountOver) {
        const uint64_t given = pictures - pictures_;
        const uint64_t missed = skipped - skipped_;
        counted_since_ = now;
        pictures_ = pictures;
        skipped_ = skipped;
        if (given >= config_.frame_rate && missed * 5 > given) {
            for (const uint32_t level : kLevels) {
                if (!below(level)) continue;
                ceiling_ = level;
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
    if (bitrate >= needs(next_lines, aspect) * kUp) {
        if (!high_since_) high_since_ = now;
    } else {
        high_since_.reset();
    }
    if (high_since_ && now - *high_since_ >= up_after_) {
        change(*next, now, pictures, skipped);
        up_at_ = now;
    }
    return lines_;
}

}  // namespace ratcast
