#pragma once

/**
 * @file video_convert.h
 * @brief Pictures from one pixel format into another, on the CPU.
 *
 * What capture gives (BGRA) into what encoders take (NV12, I420), and one
 * 4:2:0 layout into the other, in one pass over the picture written so that
 * compilers vectorise it. From BGRA, luma is exact to within a level and
 * chroma is the mean of each 2 x 2 block, sited at its centre.
 */

#include "ratcast/core/buffer.h"
#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"

namespace ratcast {

/// `picture` in `format`: always a new frame, from `pool`, with the same size
/// and capture time. YUV made from BGRA is in `color_space`; YUV into the other
/// 4:2:0 layout, or a picture already in `format`, keeps its own colour space.
/// Fails with Unsupported for YUV into BGRA.
Result<VideoFrame> convert_frame(const VideoFrame& picture, PixelFormat format,
                                 ColorSpace color_space, BufferPool& pool);
Result<VideoFrame> convert_frame(const VideoFrame& picture, PixelFormat format,
                                 ColorSpace color_space = {});

}  // namespace ratcast
