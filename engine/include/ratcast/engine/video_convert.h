#pragma once

/**
 * @file video_convert.h
 * @brief Pictures from one pixel format into another, and smaller, on the CPU.
 *
 * What capture gives (a screen's BGRA or RGBA, a camera's YUY2) into what encoders
 * take (NV12, I420), one 4:2:0 layout into the other, and a picture into a
 * smaller one for a preview, each in one pass over the picture written so that
 * compilers vectorise it. From RGB, luma is exact to within a level and chroma
 * is the mean of each 2 x 2 block, sited at its centre; from YUY2, chroma is
 * the mean of each two rows'. Back to RGB is a GPU's work; rgb_from_yuv() is
 * the matrix it needs.
 */

#include <array>

#include "ratcast/core/buffer.h"
#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"

namespace ratcast {

/// `picture` in `format`: always a new frame, from `pool`, with the same size
/// and capture time. YUV made from RGB is in `color_space`; YUV into the other
/// 4:2:0 layout, or a picture already in `format`, keeps its own colour space.
/// A picture in GPU memory is downloaded first (then, in `format` already, it
/// is that copy). Fails with Unsupported into RGB or YUY2 from another format.
Result<VideoFrame> convert_frame(const VideoFrame& picture, PixelFormat format,
                                 ColorSpace color_space, BufferPool& pool);
Result<VideoFrame> convert_frame(const VideoFrame& picture, PixelFormat format,
                                 ColorSpace color_space = {});

/// A 4:2:0 `picture` made `factor` times smaller each way, in its own format
/// and colour space, each sample the mean of a `factor` x `factor` block — what
/// a preview wants: cheap, one pass over the picture, and text still legible
/// as shapes. The size is rounded down to even; what is left over at the
/// right and the bottom is not read. `factor` 1 is the picture itself. A
/// picture in GPU memory is downloaded first. Fails with Unsupported for
/// another format, InvalidArgument for a factor of 0 or one that leaves less
/// than 2 x 2.
Result<VideoFrame> shrink_frame(const VideoFrame& picture, uint32_t factor, BufferPool& pool);
Result<VideoFrame> shrink_frame(const VideoFrame& picture, uint32_t factor);

/// The least factor shrink_frame() needs for a `width` x `height` picture to
/// fit within `max_width` x `max_height`; 1 for one that fits already.
uint32_t shrink_factor(uint32_t width, uint32_t height, uint32_t max_width,
                       uint32_t max_height) noexcept;

/// The other way, for whoever draws YUV pictures (a shader): R, G and B, each
/// a row `a, b, c, d` meaning `a·Y + b·Cb + c·Cr + d`, of samples read as a
/// GPU reads 8-bit values, from 0 to 1, and giving 0 to 1. Not clamped: limited
/// range has values beyond black and white.
using YuvToRgb = std::array<std::array<float, 4>, 3>;
YuvToRgb rgb_from_yuv(ColorSpace color_space) noexcept;

}  // namespace ratcast
