#include "ratcast/stream/frame_tracker.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace ratcast {

FrameTracker::FrameTracker(Config config) { reconfigure(config); }

void FrameTracker::reconfigure(Config config) {
    assert(config.max_pending > 0);
    config_ = config;
    pending_.reserve(config_.max_pending);
    // A push can make every pending frame ready at once, plus itself.
    ready_.reserve(config_.max_pending + 1);
}

void FrameTracker::reset(Timestamp now) {
    started_ = false;
    broken_ = false;
    last_ = 0;
    delivered_.reset();
    pending_.clear();
    ready_.clear();
    ready_head_ = 0;
    last_request_ = now;
}

void FrameTracker::push(EncodedFrame frame, Timestamp now) {
    ++stats_.received;
    const FrameNumber number = frame.info.number;

    if (!config_.in_order) {
        push_unordered(std::move(frame));
        return;
    }
    if (pending_.size() >= config_.max_pending) make_room(now);
    if (started_ && number <= last_) {
        ++stats_.late;
        return;
    }

    const auto at = std::ranges::lower_bound(pending_, number, {},
                                             [](const Pending& p) { return p.frame.info.number; });
    if (at != pending_.end() && at->frame.info.number == number) {
        ++stats_.late;  // a duplicate
        return;
    }
    pending_.insert(at, Pending{std::move(frame), now});
    advance(now);
}

void FrameTracker::update(Timestamp now) { advance(now); }

std::optional<EncodedFrame> FrameTracker::pop() {
    if (ready_head_ == ready_.size()) {
        ready_.clear();
        ready_head_ = 0;
        return std::nullopt;
    }
    return std::move(ready_[ready_head_++]);
}

void FrameTracker::invalidate(Timestamp now) {
    last_request_ = now;
    if (!started_) return;  // waiting for a keyframe already
    broken_ = true;
    delivered_.reset();  // nothing before the next keyframe can be referenced
}

bool FrameTracker::want_keyframe(Timestamp now) noexcept {
    if (decodable() || !config_.in_order) return false;
    if (last_request_ && now - *last_request_ < config_.keyframe_retry) return false;
    last_request_ = now;
    ++stats_.keyframe_requests;
    return true;
}

// A frame that stands alone goes on at once, unless it went on before.
void FrameTracker::push_unordered(EncodedFrame frame) {
    const FrameNumber number = frame.info.number;
    if (started_ && number <= last_ &&
        (last_ - number >= kReferenceWindow || delivered_[number % kReferenceWindow])) {
        ++stats_.late;
        return;
    }
    if (!started_ || number > last_) {
        // The numbers jumped over have not been seen yet; they may still come.
        if (!started_ || number - last_ >= kReferenceWindow) {
            delivered_.reset();
        } else {
            for (FrameNumber n = last_ + 1; n < number; ++n)
                delivered_[n % kReferenceWindow] = false;
        }
        started_ = true;
        last_ = number;
    }
    delivered_[number % kReferenceWindow] = true;
    ++stats_.delivered;
    ready_.push_back(std::move(frame));
}

// Hand on, or give up, whatever can be dealt with by `now`.
void FrameTracker::advance(Timestamp now) {
    while (!pending_.empty()) {
        if (jump_to_keyframe()) continue;

        const Pending& front = pending_.front();
        if (started_ && front.frame.info.number == last_ + 1) {
            // Next in sequence: every reference is behind us, so it is known
            // whether each was handed on.
            if (references_delivered(front.frame.info)) {
                deliver_front();
            } else {
                discard_front(stats_.undecodable);
                broken_ = true;
            }
            continue;
        }

        // Nothing to start from yet: wait for the keyframe, which may come
        // late. Behind a gap: wait, up to patience.
        if (!started_ || now - front.arrived < config_.patience) break;
        skip_to(front.frame.info.number - 1);
    }
}

// A decodable keyframe makes every frame before it pointless: hand it on now
// instead of waiting for gaps in front of it to fill.
bool FrameTracker::jump_to_keyframe() {
    const auto keyframe =
        std::find_if(pending_.rbegin(), pending_.rend(), [this](const Pending& p) {
            return p.frame.info.keyframe && references_delivered(p.frame.info);
        });
    if (keyframe == pending_.rend()) return false;

    const auto before = static_cast<size_t>(std::distance(keyframe, pending_.rend()) - 1);
    for (size_t i = 0; i < before; ++i) discard_front(stats_.superseded);
    skip_to(pending_.front().frame.info.number - 1);
    deliver_front();
    return true;
}

// Give up the oldest wait, so that a frame can be taken in.
void FrameTracker::make_room(Timestamp now) {
    while (pending_.size() >= config_.max_pending) {
        const FrameNumber front = pending_.front().frame.info.number;
        if (started_ && front > last_ + 1) {
            skip_to(front - 1);
        } else if (!started_) {
            discard_front(stats_.undecodable);
        }
        advance(now);
    }
}

void FrameTracker::deliver_front() {
    EncodedFrame frame = std::move(pending_.front().frame);
    pending_.erase(pending_.begin());

    const FrameNumber number = frame.info.number;
    assert(!started_ || number == last_ + 1);
    started_ = true;
    if (frame.info.keyframe) broken_ = false;
    last_ = number;
    delivered_[number % kReferenceWindow] = true;
    ++stats_.delivered;
    ready_.push_back(std::move(frame));
}

void FrameTracker::discard_front(uint64_t& counter) {
    const FrameNumber number = pending_.front().frame.info.number;
    pending_.erase(pending_.begin());
    ++counter;
    if (started_) {
        skip_to(number - 1);
        last_ = number;
        delivered_[number % kReferenceWindow] = false;
    }
}

// Move past every number up to `number`, giving up those that never came.
void FrameTracker::skip_to(FrameNumber number) {
    if (!started_) {
        last_ = number;
        delivered_.reset();
        return;
    }
    assert(number >= last_);
    const FrameNumber count = number - last_;
    stats_.lost += count;
    if (count >= kReferenceWindow) {
        delivered_.reset();
    } else {
        for (FrameNumber n = last_ + 1; n <= number; ++n) delivered_[n % kReferenceWindow] = false;
    }
    last_ = number;
}

bool FrameTracker::references_delivered(const FrameInfo& info) const noexcept {
    return std::ranges::all_of(info.references,
                               [this](FrameNumber reference) { return was_delivered(reference); });
}

bool FrameTracker::was_delivered(FrameNumber number) const noexcept {
    return started_ && number <= last_ && last_ - number < kReferenceWindow &&
           delivered_[number % kReferenceWindow];
}

}  // namespace ratcast
