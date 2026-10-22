#pragma once

/**
 * @file hw_frames.h
 * @brief Pictures in GPU memory as FFmpeg's hardware frames.
 *
 * An encoder that takes a GPU's textures is opened on a frames context of
 * that GPU's device, and each picture is copied into a frame of it on the GPU
 * itself — or uploaded, for a picture in memory. What is done per graphics
 * API lives in a file of its own (hw_frames_d3d11.cpp); where there is none,
 * no device has frames and every encoder takes pictures from memory.
 */

#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/buffer.h>
}

#include "ffmpeg/av.h"
#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"

namespace ratcast::av {

struct BufferRefDeleter {
    void operator()(AVBufferRef* ref) const noexcept { av_buffer_unref(&ref); }
};
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;

/// Whether `codec` takes frames of `device`'s, in a frames context.
bool takes_hw_frames(const AVCodec& codec, const GpuDevice& device) noexcept;

/// A frames context of `device`'s for `width` x `height` NV12 pictures.
Result<BufferRefPtr> make_hw_frames(const std::shared_ptr<GpuDevice>& device, uint32_t width,
                                    uint32_t height);

/// `picture`, NV12 and of the frames' size, into a new frame of `frames`'s in
/// `frame`: copied on the GPU if it is in the frames' device's memory,
/// uploaded otherwise.
Result<void> fill_hw_frame(AVBufferRef& frames, const VideoFrame& picture, AVFrame& frame);

}  // namespace ratcast::av
