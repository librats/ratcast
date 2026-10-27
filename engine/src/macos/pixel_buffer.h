#pragma once

/**
 * @file pixel_buffer.h
 * @brief Core Video's pixel buffers as frames, with no copy.
 *
 * What ScreenCaptureKit and AVFoundation hand out: a buffer from their pool,
 * which goes back to it once let go of. The frame holds it for as long as any
 * copy of the frame lives — so a sink that keeps pictures keeps the pool
 * short, as with any capture API.
 *
 * A frame over its memory locks it for reading (frame_of()). A frame of a
 * buffer on an IOSurface may instead carry it as a picture in GPU memory
 * (gpu_frame_of()), unlocked, for VideoToolbox to read where it is; the GPU's
 * memory is the CPU's on Apple's machines, so anything else reads it in place
 * too (GpuPicture::mapped()), with no copy either way.
 */

#include <CoreVideo/CoreVideo.h>

#include <memory>

#include "ratcast/core/time.h"
#include "ratcast/engine/video_frame.h"

namespace ratcast::macos {

/// `pixels` — NV12 ('420v', '420f') or BGRA — as a frame over its memory,
/// 4:2:0 sizes made even; empty for any other format or a buffer that will
/// not lock. A YUV frame is in `color_space`.
VideoFrame frame_of(CVPixelBufferRef pixels, Timestamp captured, ColorSpace color_space);

/// `pixels` — NV12 of even sizes, on an IOSurface — as a picture in GPU
/// memory (GpuApi::CoreVideo) in `color_space`; empty for any other buffer.
VideoFrame gpu_frame_of(CVPixelBufferRef pixels, Timestamp captured, ColorSpace color_space);

/// The pixel buffer a picture of gpu_frame_of() is, or null.
CVPixelBufferRef pixel_buffer_of(const VideoFrame& picture) noexcept;

/// The device every picture of gpu_frame_of() belongs to: one GPU, whose
/// memory is the CPU's.
const std::shared_ptr<GpuDevice>& core_video_device();

/// The colour space `pixels` says it is in: its range by its format, its
/// matrix by its attachment, BT.601 when it has none.
ColorSpace color_space_of(CVPixelBufferRef pixels);

}  // namespace ratcast::macos
