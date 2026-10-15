#pragma once

/**
 * @file frame.h
 * @brief An encoded frame and what a receiver must know to decode it in time.
 *
 * libratcast moves encoded frames and never looks inside them. What it needs is
 * the metadata: which stream, which frame, when it was captured, whether a
 * decoder can start from it, and which earlier frames it cannot be decoded
 * without. The last is what lets a receiver drop a frame whose reference was
 * lost instead of feeding a decoder garbage, and lets a forwarder drop a whole
 * temporal or spatial layer for a receiver that cannot take it, all without
 * understanding a single codec.
 */

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "ratcast/buffer.h"
#include "ratcast/time.h"

namespace ratcast {

/// Identifies a stream within a session.
using StreamId = uint32_t;

/// Position of a frame in its stream, in decode order, counting from 0. Never
/// wraps: at 240 frames per second 64 bits outlast any session.
using FrameNumber = uint64_t;

/// The earlier frames of the same stream a frame references. Fixed capacity, so
/// frame metadata is a plain value that never allocates.
class FrameReferences {
public:
    /// More than any codec references in a real-time configuration (AV1 has
    /// seven reference slots, rarely more than three distinct frames).
    static constexpr size_t kCapacity = 8;

    FrameReferences() noexcept = default;

    /// Precondition: at most kCapacity entries.
    FrameReferences(std::initializer_list<FrameNumber> frames) noexcept {
        assert(frames.size() <= kCapacity);
        for (FrameNumber frame : frames) push_back(frame);
    }

    /// Add `frame`; false, changing nothing, when already full.
    bool push_back(FrameNumber frame) noexcept {
        if (size_ == kCapacity) return false;
        frames_[size_++] = frame;
        return true;
    }

    void clear() noexcept { size_ = 0; }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    FrameNumber operator[](size_t i) const noexcept {
        assert(i < size_);
        return frames_[i];
    }
    const FrameNumber* begin() const noexcept { return frames_.data(); }
    const FrameNumber* end() const noexcept { return frames_.data() + size_; }

    friend bool operator==(const FrameReferences& a, const FrameReferences& b) noexcept {
        if (a.size_ != b.size_) return false;
        for (size_t i = 0; i < a.size_; ++i) {
            if (a.frames_[i] != b.frames_[i]) return false;
        }
        return true;
    }

private:
    std::array<FrameNumber, kCapacity> frames_{};
    uint8_t size_ = 0;
};

/// Everything about a frame except its bytes.
struct FrameInfo {
    static constexpr uint8_t kMaxTemporalLayer = 7;
    static constexpr uint8_t kMaxSpatialLayer = 3;

    StreamId stream = 0;
    FrameNumber number = 0;
    /// When the content of the frame was captured, on the sender's clock.
    Timestamp capture_time{};
    /// A decoder can start here: nothing before this frame is needed for it or
    /// for anything after it.
    bool keyframe = false;
    uint8_t temporal_layer = 0;  ///< 0 (base) .. kMaxTemporalLayer
    uint8_t spatial_layer = 0;   ///< 0 (base) .. kMaxSpatialLayer
    /// Earlier frames this one cannot be decoded without; each is < number.
    FrameReferences references;

    friend bool operator==(const FrameInfo&, const FrameInfo&) = default;
};

/// An encoded frame as a receiver hands it on. The payload is typically a
/// slice of the received message, so holding it keeps that message alive.
struct EncodedFrame {
    FrameInfo info;
    Buffer payload;
};

/// Headroom an encoder leaves in front of a payload, so the wire header can be
/// written in place and the frame sent without copying it. Enough for the
/// largest frame header (static_assert in the wire codec).
inline constexpr size_t kFrameHeadroom = 128;

/// Storage for an encoder to write a frame of up to `capacity` bytes into.
inline MutableBuffer allocate_frame_payload(size_t capacity) {
    return MutableBuffer::allocate(capacity, kFrameHeadroom);
}

}  // namespace ratcast
