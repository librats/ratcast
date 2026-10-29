#include "ratcast/engine/video_codec.h"

#include "video/codec_backend.h"

#ifdef __ANDROID__
#include "android/mediacodec_decoder.h"
#endif

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
#ifdef __ANDROID__
    // The device's hardware first: software decoding takes a phone's
    // processors and battery.
    if (Result<std::unique_ptr<VideoDecoder>> decoder = android::open_mediacodec_decoder(config))
        return decoder;
#endif
    return backend::open_video_decoder(config);
}

std::vector<Codec> live_video_decoders() {
    static const std::vector<Codec> codecs =
#ifdef __ANDROID__
        android::hardware_video_decoders();
#else
        backend::video_decoders();
#endif
    return codecs;
}

void set_codec_logging(bool verbose) { backend::set_codec_logging(verbose); }

}  // namespace ratcast
