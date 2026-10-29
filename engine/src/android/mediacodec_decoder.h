#pragma once

/**
 * @file mediacodec_decoder.h
 * @brief Video decoding by the device's hardware, through MediaCodec's NDK API.
 *
 * Only a hardware decoder is taken: the system's software ones (c2.android.*,
 * OMX.google.*) do no better than FFmpeg's, which open_video_decoder() falls
 * back to. Set up for live input — low latency (Android 11's key, and
 * Qualcomm's own) — and read in byte buffers, each picture copied out of the
 * decoder's buffer into a pooled 4:2:0 frame, so that the few buffers a
 * decoder has go back to it at once. decode() waits a little for the picture
 * of the frame it was given, as the decoder works on its own.
 */

#include <memory>

#include "ratcast/core/result.h"
#include "ratcast/engine/video_codec.h"

namespace ratcast::android {

/// A hardware decoder of `config.codec`; Unavailable where the device has none.
Result<std::unique_ptr<VideoDecoder>> open_mediacodec_decoder(const VideoDecoderConfig& config);

}  // namespace ratcast::android
