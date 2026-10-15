#include "ratcast/engine/video_codec.h"

#include "video/codec_backend.h"

namespace ratcast {
namespace {

bool is_valid(const VideoEncoderConfig& config) {
    // 4:2:0 halves both dimensions, which encoders want whole.
    return media_kind_of(config.codec) == MediaKind::Video && config.width > 0 &&
           config.height > 0 && config.width % 2 == 0 && config.height % 2 == 0 &&
           config.frame_rate_num > 0 && config.frame_rate_den > 0 && config.bitrate > 0;
}

}  // namespace

std::vector<std::string> video_encoder_candidates(Codec codec) {
    return backend::video_encoder_candidates(codec);
}

Result<std::unique_ptr<VideoEncoder>> open_video_encoder(const VideoEncoderConfig& config) {
    if (!is_valid(config)) return fail(Errc::InvalidArgument);
    return backend::open_video_encoder(config);
}

Result<std::unique_ptr<VideoDecoder>> open_video_decoder(const VideoDecoderConfig& config) {
    if (media_kind_of(config.codec) != MediaKind::Video) return fail(Errc::InvalidArgument);
    return backend::open_video_decoder(config);
}

}  // namespace ratcast
