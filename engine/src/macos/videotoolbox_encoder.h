#pragma once

/**
 * @file videotoolbox_encoder.h
 * @brief H.264 and HEVC on the GPU's encoder through VideoToolbox's own API.
 *
 * FFmpeg's wrapper of VideoToolbox hands a picture to the session and returns
 * whatever the session had finished by then, so each frame comes out a call
 * late. Here the call waits for the picture it took, as every encoder here
 * must; the encoder table (ffmpeg/video_encoder.cpp) opens it under FFmpeg's
 * names, h264_videotoolbox and hevc_videotoolbox. Opened for the device of
 * Core Video's pixel buffers (GpuApi::CoreVideo), it takes a capture's
 * IOSurfaces as they are (VideoEncoderInfo::gpu_input).
 */

#include <memory>

#include "ratcast/core/result.h"
#include "ratcast/engine/video_codec.h"

namespace ratcast::macos {

/// A hardware encoder of `config.codec` (H.264 or HEVC). Fails with
/// Unavailable where there is none.
Result<std::unique_ptr<VideoEncoder>> open_videotoolbox_encoder(const VideoEncoderConfig& config);

}  // namespace ratcast::macos
