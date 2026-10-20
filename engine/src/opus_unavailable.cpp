// What libopus provides, in a build without it: there is nothing to open.

#include "audio/audio_backend.h"

namespace ratcast::backend {

Result<std::unique_ptr<AudioEncoder>> open_audio_encoder(const AudioEncoderConfig&) {
    return fail(Errc::Unavailable);
}

Result<std::unique_ptr<AudioDecoder>> open_audio_decoder(const AudioDecoderConfig&) {
    return fail(Errc::Unavailable);
}

}  // namespace ratcast::backend
