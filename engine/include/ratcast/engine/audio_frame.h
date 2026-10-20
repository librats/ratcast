#pragma once

/**
 * @file audio_frame.h
 * @brief A block of raw sound: what sources produce, encoders take and decoders give.
 *
 * Samples are 32-bit floats in [-1, 1], interleaved by channel (left, right,
 * left, …): what PipeWire hands out natively, what Opus takes, and what audio
 * processing works in. A block is any length; how much a source hands out at a
 * time is the source's affair, and the encoder cuts whatever it gets into
 * frames of its own.
 *
 * Like VideoFrame, an AudioFrame is a value, cheap to copy: copies share the
 * pooled memory of the samples, which is filled once in a MutableAudioFrame
 * and never written again.
 */

#include <cstddef>
#include <cstdint>
#include <span>

#include "ratcast/core/buffer.h"
#include "ratcast/core/time.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast {

/// How long `length` samples per channel last at `sample_rate`, to the
/// microsecond below.
constexpr Duration audio_duration(size_t length, uint32_t sample_rate) noexcept {
    return Duration{static_cast<int64_t>(length) * 1'000'000 / sample_rate};
}

/// How many samples per channel `duration` holds at `sample_rate`, rounded down.
constexpr size_t audio_length(Duration duration, uint32_t sample_rate) noexcept {
    return static_cast<size_t>(duration.count() * sample_rate / 1'000'000);
}

class AudioFrame {
public:
    /// An empty frame: no sound.
    AudioFrame() noexcept = default;

    bool empty() const noexcept { return length_ == 0; }
    const AudioFormat& format() const noexcept { return format_; }
    /// Samples per channel.
    size_t length() const noexcept { return length_; }
    Duration duration() const noexcept { return audio_duration(length_, format_.sample_rate); }

    /// length() * channels samples, interleaved.
    std::span<const float> samples() const noexcept { return {data_, length_ * format_.channels}; }

    /// When the first sample was captured, on the local monotonic clock.
    Timestamp capture_time() const noexcept { return capture_time_; }
    void set_capture_time(Timestamp time) noexcept { capture_time_ = time; }

private:
    friend class MutableAudioFrame;

    AudioFormat format_;
    size_t length_ = 0;
    const float* data_ = nullptr;
    Timestamp capture_time_{};
    Buffer storage_;
};

/// A block being filled: writable samples in pooled memory, frozen into an
/// AudioFrame once complete. Move-only.
class MutableAudioFrame {
public:
    /// `length` samples per channel of `format`, contents unspecified.
    /// Precondition: format.channels > 0 and format.sample_rate > 0.
    static MutableAudioFrame allocate(AudioFormat format, size_t length);
    static MutableAudioFrame allocate(AudioFormat format, size_t length, BufferPool& pool);

    MutableAudioFrame(MutableAudioFrame&&) noexcept = default;
    MutableAudioFrame& operator=(MutableAudioFrame&&) noexcept = default;

    const AudioFormat& format() const noexcept { return format_; }
    size_t length() const noexcept { return length_; }
    std::span<float> samples() noexcept { return {data_, length_ * format_.channels}; }

    /// The finished block, its first sample captured at `capture_time`.
    /// Leaves this empty.
    AudioFrame freeze(Timestamp capture_time) && noexcept;

private:
    MutableAudioFrame() = default;

    AudioFormat format_;
    size_t length_ = 0;
    float* data_ = nullptr;
    MutableBuffer storage_;
};

}  // namespace ratcast
