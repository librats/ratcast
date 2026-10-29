// What FFmpeg provides, in a build without it: there is nothing to open.

#include "ratcast/engine/file_source.h"
#include "video/codec_backend.h"

namespace ratcast {

Result<std::unique_ptr<FileSource>> open_file_source(const FileSourceOptions&, Clock&) {
    return fail(Errc::Unavailable);
}

}  // namespace ratcast

namespace ratcast::backend {

std::vector<std::string> video_encoder_candidates(Codec) { return {}; }

Result<std::unique_ptr<VideoEncoder>> open_video_encoder(const VideoEncoderConfig&) {
    return fail(Errc::Unavailable);
}

Result<std::unique_ptr<VideoDecoder>> open_video_decoder(const VideoDecoderConfig&) {
    return fail(Errc::Unavailable);
}

std::vector<Codec> video_decoders() { return {}; }

void set_codec_logging(bool) {}

}  // namespace ratcast::backend
