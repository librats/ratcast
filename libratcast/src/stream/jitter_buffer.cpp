#include "ratcast/stream/jitter_buffer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>

namespace ratcast {
namespace {

using namespace std::chrono_literals;

/// Capture times are whole microseconds, cut from sample counts: a frame
/// within this of the cursor is on it.
constexpr Duration kTolerance = 1ms;

/// The least gap worth making up: a decoder's unit (Opus's), shorter gaps
/// coming out of it as nothing.
constexpr Duration kLeastGap = 2500us;

/// A delay over target by more than a frame and a quarter (so that one frame
/// fewer leaves it still over, by more than the few milliseconds a pull's
/// timing moves it), for kOverFor together, costs a frame every kDropEvery
/// until it is not; one under the floor by more than a frame has a frame made
/// up as often. By a frame and a half, as it was, a delay up to 30 ms over its
/// target stayed there for good in sound that never pauses.
constexpr Duration kOverFor = 1s;
constexpr Duration kDropEvery = 100ms;

}  // namespace

JitterBuffer::JitterBuffer(Config config)
    : config_(config),
      fastest_(config.memory),
      target_(config.min_delay),
      wanted_(config.min_delay) {
    assert(config.min_delay <= config.max_delay && config.max_frames > 0);
    frames_.reserve(config.max_frames + 1);
}

void JitterBuffer::reset() {
    const Config config = config_;
    const Duration floor = floor_;
    *this = JitterBuffer(config);
    set_min_delay(floor);
}

void JitterBuffer::set_min_delay(Duration floor) noexcept {
    floor = std::min(floor, config_.max_delay);
    if (floor == floor_) return;  // asked for again and again, by every pull
    floor_ = floor;
    if (last_arrival_) {
        update_target();
    } else {
        target_ = std::clamp(floor_, config_.min_delay, config_.max_delay);
    }
}

void JitterBuffer::push(EncodedFrame frame, Timestamp now) {
    ++stats_.received;
    learn(frame, now);
    if (!first_arrival_) first_arrival_ = now;

    const FrameNumber number = frame.info.number;
    if (playing_ && (number < next_number_ || frame.info.capture_time + kTolerance < cursor_)) {
        ++stats_.late;
        return;
    }
    const auto at = std::ranges::lower_bound(frames_, number, {},
                                             [](const EncodedFrame& f) { return f.info.number; });
    if (at != frames_.end() && at->info.number == number) {
        ++stats_.late;  // a duplicate
        return;
    }
    frames_.insert(at, std::move(frame));
    if (frames_.size() > config_.max_frames) {
        frames_.erase(frames_.begin());  // the player is not taking them
        ++stats_.dropped;
    }
}

// What the frame says of the path: its transit, against the fastest lately,
// and of the sender: how long its frames are.
void JitterBuffer::learn(const EncodedFrame& frame, Timestamp now) {
    if (last_pushed_ && frame.info.number == last_pushed_->number + 1) {
        const Duration step = frame.info.capture_time - last_pushed_->capture_time;
        // The least step between neighbours: a larger one is silence left out.
        if (step > Duration::zero()) {
            frame_duration_ = learnt_ ? std::min(frame_duration_, step) : step;
            learnt_ = true;
        }
    }
    last_pushed_ = frame.info;

    const Duration transit = now - frame.info.capture_time;
    fastest_.add(transit, now);

    // Lateness beyond the base, into a histogram whose past fades over memory.
    const double fade = last_arrival_
                            ? std::exp(-std::chrono::duration<double>(now - *last_arrival_) /
                                       std::chrono::duration<double>(config_.memory))
                            : 1.0;
    last_arrival_ = now;
    for (double& weight : lateness_) weight *= fade;
    const auto bucket =
        static_cast<size_t>(std::max(transit - *fastest_.least(), Duration::zero()) / kBucket);
    lateness_[std::min(bucket, kBuckets - 1)] += 1.0;
    update_target();
}

// The lateness that `in_time` of what is remembered keeps within, and a frame
// more: by the time a lost frame is due, the one after it is there, and its
// FEC can stand in. The floor, if higher.
void JitterBuffer::update_target() {
    double total = 0;
    for (double weight : lateness_) total += weight;
    double within = 0;
    size_t bucket = 0;
    for (; bucket < kBuckets - 1; ++bucket) {
        within += lateness_[bucket];
        if (within >= config_.in_time * total) break;
    }
    wanted_ = std::clamp(kBucket * static_cast<int64_t>(bucket + 1) + frame_duration_,
                         config_.min_delay, config_.max_delay);
    target_ = std::clamp(std::max(wanted_, floor_), config_.min_delay, config_.max_delay);
}

JitterBuffer::Next JitterBuffer::next(Timestamp now) {
    if (!playing_) {
        // The first frame waits the target out before it plays.
        if (frames_.empty() || now - *first_arrival_ < target_) return {};
        playing_ = true;
        cursor_ = frames_.front().info.capture_time;
        next_number_ = frames_.front().info.number;
    }

    for (;;) {
        delay_ = now - cursor_ - *fastest_.least();
        // Frames the cursor went past while they waited (it cut silence, or
        // dropped a frame) are of no use now.
        while (!frames_.empty() && frames_.front().info.capture_time + kTolerance < cursor_) {
            frames_.erase(frames_.begin());
            ++stats_.late;
        }
        const Duration frame = frame_duration_;

        if (frames_.empty()) {
            // Nothing there: lost, late, or silence until the next frame says
            // which. Short of target, the cursor stays, and the delay grows
            // to it: all of it, or the frame after a lost one may not be there
            // with its FEC when the lost one is due.
            if (delay_ < target_) {
                const Duration wait = std::clamp(target_ - delay_, kLeastGap, frame);
                stats_.expanded += wait;
                return gap(wait, std::nullopt, false);
            }
            stats_.unexplained += frame;
            return gap(frame, std::nullopt, true);
        }

        const EncodedFrame& front = frames_.front();
        const Duration ahead = front.info.capture_time - cursor_;
        if (ahead <= kTolerance) {
            if (stretch(now)) {
                stats_.expanded += frame;
                return gap(frame, std::nullopt, false);
            }
            if (std::optional<Next> played = play_front(now)) return std::move(*played);
            continue;  // dropped to bring the delay down
        }

        if (front.info.number > next_number_) {
            // Frames before it are missing. Short of target they may yet come:
            // wait, and the delay grows. Otherwise they are lost: made up, the
            // last of them with the FEC that `front` carries.
            if (delay_ < target_) {
                const Duration wait = std::clamp(target_ - delay_, kLeastGap, frame);
                stats_.expanded += wait;
                return gap(wait, std::nullopt, false);
            }
            const Duration piece = std::min(ahead, frame);
            stats_.concealed += piece;
            return gap(piece, piece == ahead ? std::optional(front) : std::nullopt, true);
        }

        // Silence: where the delay is adjusted without a sound.
        if (delay_ > target_ + frame / 2) {
            const Duration cut = std::min(ahead, delay_ - target_);
            cursor_ += cut;
            stats_.cut += cut;
            continue;
        }
        const Duration piece = std::min(ahead, frame);
        if (delay_ < target_ - frame / 2) {
            stats_.expanded += piece;
            return gap(piece, std::nullopt, false);
        }
        stats_.silence += piece;
        return gap(piece, std::nullopt, true);
    }
}

// The front frame, due now; nullopt when it was dropped instead, the delay
// having been well over target for long enough.
std::optional<JitterBuffer::Next> JitterBuffer::play_front(Timestamp now) {
    EncodedFrame frame = std::move(frames_.front());
    frames_.erase(frames_.begin());
    const FrameNumber number = frame.info.number;
    // What was made up before it while nothing was there: for the frames
    // missing before it, or silence.
    if (number > next_number_) {
        stats_.lost += number - next_number_;
        stats_.concealed += stats_.unexplained;
    } else {
        stats_.silence += stats_.unexplained;
    }
    stats_.unexplained = Duration::zero();
    cursor_ = std::max(cursor_, frame.info.capture_time) + frame_duration_;
    next_number_ = number + 1;

    if (delay_ <= target_ + frame_duration_ * 5 / 4) {
        over_since_.reset();
    } else if (!over_since_) {
        over_since_ = now;
    } else if (now - *over_since_ >= kOverFor && !frames_.empty()) {
        over_since_ = now - kOverFor + kDropEvery;
        ++stats_.dropped;
        return std::nullopt;
    }

    ++stats_.played;
    Next next;
    next.kind = Next::Kind::Frame;
    next.capture_time = frame.info.capture_time;
    next.duration = frame_duration_;
    next.frame = std::move(frame);
    return next;
}

// Whether to make up a frame's worth before the front frame, due now, the
// delay having been well short of the floor for long enough.
bool JitterBuffer::stretch(Timestamp now) {
    if (delay_ + frame_duration_ >= floor_) {
        under_since_.reset();
        return false;
    }
    if (!under_since_) under_since_ = now;
    if (now - *under_since_ < kOverFor) return false;
    under_since_ = now - kOverFor + kDropEvery;
    return true;
}

JitterBuffer::Next JitterBuffer::gap(Duration duration, std::optional<EncodedFrame> after,
                                     bool advance) {
    Next next;
    next.kind = Next::Kind::Gap;
    next.capture_time = cursor_;
    next.duration = duration;
    next.frame = std::move(after);
    if (advance) cursor_ += duration;
    return next;
}

}  // namespace ratcast
