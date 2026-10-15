// The codec backend of a build without FFmpeg: there is nothing to open.

#include "video/codec_backend.h"

namespace ratcast::backend {

std::vector<std::string> video_encoder_candidates(Codec) { return {}; }

Result<std::unique_ptr<VideoEncoder>> open_video_encoder(const VideoEncoderConfig&) {
    return fail(Errc::Unavailable);
}

Result<std::unique_ptr<VideoDecoder>> open_video_decoder(const VideoDecoderConfig&) {
    return fail(Errc::Unavailable);
}

}  // namespace ratcast::backend
