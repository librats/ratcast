#pragma once

/**
 * @file hardware_picture.h
 * @brief Android's AHardwareBuffers as pictures in GPU memory, with no copy.
 *
 * What an AImageReader made for the GPU hands out — a VirtualDisplay drawing
 * the screen into it, a decoder its pictures — is a buffer of its queue,
 * given back once the image holding it is let go of. A frame carries one as a
 * picture in GPU memory (GpuApi::HardwareBuffer), held with whatever gives it
 * back for as long as any copy of the frame lives, so a sink that keeps
 * pictures keeps the queue short. MediaCodec's encoder draws it into its
 * input surface where it is (android/surface_drawer.h), the app's view draws
 * it too (hardware_buffer_of(), ratcast/engine/android.h); anything else
 * reads it locked for the CPU (GpuPicture::mapped(), else a copy), which a
 * buffer made for the CPU to read too allows. RGBA, what the screen gives,
 * or YUV 4:2:0 in the layout its allocator chose (a decoder's), read as NV12.
 */

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>

#include <memory>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/android.h"
#include "ratcast/engine/video_frame.h"

namespace ratcast::android {

/// The device every picture of gpu_frame_of() belongs to.
const std::shared_ptr<GpuDevice>& hardware_buffer_device();

/// The picture of `image`, an AImageReader's — RGBA_8888, or YUV 4:2:0 in
/// any layout (Y8Cb8Cr8_420, a vendor's own), read through the image — as a
/// picture in GPU memory, its crop (from the top left, made even), in
/// `color_space` (that an RGBA one becomes). The frame holds the buffer and
/// `owner`, which keeps the image and gives it back. Empty for an image
/// without a buffer or cropped elsewhere.
VideoFrame gpu_frame_of(AImage* image, std::shared_ptr<const void> owner, Timestamp captured,
                        ColorSpace color_space);

/// `picture`, RGBA in memory, copied into a new buffer the GPU reads, as a
/// picture of gpu_frame_of() in its colour space: what tests draw.
Result<VideoFrame> upload(const VideoFrame& picture);

}  // namespace ratcast::android
