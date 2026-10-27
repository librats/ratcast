#pragma once

/**
 * @file pixel_buffer.h
 * @brief Core Video's pixel buffers as frames, with no copy.
 *
 * What ScreenCaptureKit and AVFoundation hand out: a buffer from their pool,
 * which goes back to it once let go of. The frame locks it for reading and
 * holds it for as long as any copy of the frame lives — so a sink that keeps
 * pictures keeps the pool short, as with any capture API.
 */

#include <CoreVideo/CoreVideo.h>

#include "ratcast/core/time.h"
#include "ratcast/engine/video_frame.h"

namespace ratcast::macos {

/// `pixels` — NV12 ('420v', '420f') or BGRA — as a frame over its memory,
/// 4:2:0 sizes made even; empty for any other format or a buffer that will
/// not lock. A YUV frame is in `color_space`.
VideoFrame frame_of(CVPixelBufferRef pixels, Timestamp captured, ColorSpace color_space);

/// The colour space `pixels` says it is in: its range by its format, its
/// matrix by its attachment, BT.601 when it has none.
ColorSpace color_space_of(CVPixelBufferRef pixels);

}  // namespace ratcast::macos
