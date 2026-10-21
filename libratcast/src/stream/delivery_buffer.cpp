#include "ratcast/stream/delivery_buffer.h"

#include <algorithm>
#include <utility>

namespace ratcast {

DeliveryBuffer::DeliveryBuffer(Config config) noexcept : config_(config), fastest_(config.memory) {}

void DeliveryBuffer::reset() noexcept { *this = DeliveryBuffer(config_); }

void DeliveryBuffer::push(EncodedFrame frame, Timestamp now) {
    ++stats_.received;
    fastest_.add(now - frame.info.capture_time, now);

    const FrameNumber number = frame.info.number;
    if (next_number_ && number < *next_number_) {
        ++stats_.late;
        return;
    }
    // Mostly in order: the place is near the back.
    const auto at = std::upper_bound(
        frames_.begin(), frames_.end(), number,
        [](FrameNumber n, const EncodedFrame& held) { return n < held.info.number; });
    if (at != frames_.begin() && std::prev(at)->info.number == number) {
        ++stats_.late;  // a duplicate
        return;
    }
    frames_.insert(at, std::move(frame));
}

std::optional<EncodedFrame> DeliveryBuffer::pop(Timestamp now) {
    if (frames_.empty()) return std::nullopt;
    const bool room = frames_.size() > config_.max_frames;
    if (!room && due(frames_.front()) > now) return std::nullopt;

    EncodedFrame frame = std::move(frames_.front());
    frames_.pop_front();
    const FrameNumber number = frame.info.number;
    if (next_number_ && number > *next_number_) stats_.lost += number - *next_number_;
    next_number_ = number + 1;
    ++stats_.delivered;
    if (room) ++stats_.early;
    return frame;
}

std::optional<Timestamp> DeliveryBuffer::next_due() const noexcept {
    if (frames_.empty()) return std::nullopt;
    return due(frames_.front());
}

Timestamp DeliveryBuffer::due(const EncodedFrame& frame) const noexcept {
    return frame.info.capture_time + *fastest_.least() + config_.latency;
}

}  // namespace ratcast
