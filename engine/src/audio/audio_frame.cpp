#include "ratcast/engine/audio_frame.h"

#include <cassert>
#include <cstdint>

namespace ratcast {

MutableAudioFrame MutableAudioFrame::allocate(AudioFormat format, size_t length) {
    return allocate(format, length, BufferPool::shared());
}

MutableAudioFrame MutableAudioFrame::allocate(AudioFormat format, size_t length, BufferPool& pool) {
    assert(format.channels > 0 && format.sample_rate > 0);
    MutableAudioFrame frame;
    frame.format_ = format;
    frame.length_ = length;
    const size_t bytes = length * format.channels * sizeof(float);
    frame.storage_ = MutableBuffer::allocate(bytes, 0, pool);
    // Pooled blocks start on the allocator's alignment, which holds a float.
    frame.data_ = reinterpret_cast<float*>(frame.storage_.append(bytes));
    assert(reinterpret_cast<uintptr_t>(frame.data_) % alignof(float) == 0);
    return frame;
}

AudioFrame MutableAudioFrame::freeze(Timestamp capture_time) && noexcept {
    AudioFrame frame;
    frame.format_ = format_;
    frame.length_ = length_;
    frame.data_ = data_;
    frame.capture_time_ = capture_time;
    frame.storage_ = std::move(storage_).freeze();
    *this = MutableAudioFrame{};
    return frame;
}

}  // namespace ratcast
