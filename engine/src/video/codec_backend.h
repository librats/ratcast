#pragma once

/**
 * @file codec_backend.h
 * @brief What a codec backend provides behind video_codec.h.
 *
 * The public entry points check their arguments and hand valid requests here,
 * so every backend — FFmpeg, or none at all — answers the same way to the
 * same mistakes.
 */

#include <memory>
#include <string>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/engine/video_codec.h"

namespace ratcast::backend {

std::vector<std::string> video_encoder_candidates(Codec codec);

/// Precondition: `config` passed open_video_encoder()'s checks.
Result<std::unique_ptr<VideoEncoder>> open_video_encoder(const VideoEncoderConfig& config);

/// Precondition: `config.codec` is a video codec.
Result<std::unique_ptr<VideoDecoder>> open_video_decoder(const VideoDecoderConfig& config);

}  // namespace ratcast::backend
