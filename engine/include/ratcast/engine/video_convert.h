#pragma once

/**
 * @file video_convert.h
 * @brief Pictures from one pixel format into another, on the CPU.
 *
 * What capture gives (a screen's BGRA, a camera's YUY2) into what encoders
 * take (NV12, I420), and one 4:2:0 layout into the other, in one pass over the
 * picture written so that compilers vectorise it. From BGRA, luma is exact to
 * within a level and chroma is the mean of each 2 x 2 block, sited at its
 * centre; from YUY2, chroma is the mean of each two rows'. Back to RGB is
 * a GPU's work; rgb_from_yuv() is the matrix it needs.
 */

#include <array>

#include "ratcast/core/buffer.h"
#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"

namespace ratcast {

/// `picture` in `format`: always a new frame, from `pool`, with the same size
/// and capture time. YUV made from BGRA is in `color_space`; YUV into the other
/// 4:2:0 layout, or a picture already in `format`, keeps its own colour space.
/// Fails with Unsupported into BGRA or YUY2 from another format.
Result<VideoFrame> convert_frame(const VideoFrame& picture, PixelFormat format,
                                 ColorSpace color_space, BufferPool& pool);
Result<VideoFrame> convert_frame(const VideoFrame& picture, PixelFormat format,
                                 ColorSpace color_space = {});

/// The other way, for whoever draws YUV pictures (a shader): R, G and B, each
/// a row `a, b, c, d` meaning `a·Y + b·Cb + c·Cr + d`, of samples read as a
/// GPU reads 8-bit values, from 0 to 1, and giving 0 to 1. Not clamped: limited
/// range has values beyond black and white.
using YuvToRgb = std::array<std::array<float, 4>, 3>;
YuvToRgb rgb_from_yuv(ColorSpace color_space) noexcept;

}  // namespace ratcast
