#pragma once

/**
 * @file mediacodec_encoder.h
 * @brief H.264 and HEVC on the device's hardware encoder, through MediaCodec's
 * NDK API.
 *
 * FFmpeg's wrapper of MediaCodec takes pictures through a Surface or hands
 * frames out as they come, a few late; here each picture is copied into the
 * encoder's own buffer, laid out as its input format says, and its frame
 * waited for, so that the call that took it hands it out, as every encoder
 * here must. Set up for live use: constant bitrate, no B-frames, one picture
 * in flight at most (Android 11's latency key), keyframes only when asked
 * (`request-sync`), the bitrate changed in flight (`video-bitrate`). Frames
 * leave in Annex B with a keyframe's parameter sets ahead of it. The encoder
 * table (ffmpeg/video_encoder.cpp) opens it under FFmpeg's names,
 * h264_mediacodec and hevc_mediacodec. Only a hardware encoder is taken: the
 * system's software ones do no better than OpenH264.
 */

#include <memory>

#include "ratcast/core/result.h"
#include "ratcast/engine/video_codec.h"

namespace ratcast::android {

/// A hardware encoder of `config.codec` (H.264 or HEVC). Fails with
/// Unavailable where there is none, or it will not take the config.
Result<std::unique_ptr<VideoEncoder>> open_mediacodec_encoder(const VideoEncoderConfig& config);

}  // namespace ratcast::android
