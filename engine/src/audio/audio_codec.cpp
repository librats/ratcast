#include "ratcast/engine/audio_codec.h"

#include "audio/audio_backend.h"

namespace ratcast {
namespace {

using std::chrono::milliseconds;

// What Opus takes and gives: its internal rates, mono or stereo.
bool is_valid(const AudioFormat& format) {
    switch (format.sample_rate) {
        case 8'000:
        case 12'000:
        case 16'000:
        case 24'000:
        case 48'000: break;
        default: return false;
    }
    return format.channels == 1 || format.channels == 2;
}

bool is_valid(const AudioEncoderConfig& config) {
    const Duration d = config.frame_duration;
    return config.codec == Codec::Opus && is_valid(config.format) &&
           (d == milliseconds(10) || d == milliseconds(20) || d == milliseconds(40) ||
            d == milliseconds(60)) &&
           config.bitrate >= 6'000 && config.bitrate <= 510'000 && config.expected_loss <= 100;
}

}  // namespace

Result<std::unique_ptr<AudioEncoder>> open_audio_encoder(const AudioEncoderConfig& config) {
    if (!is_valid(config)) return fail(Errc::InvalidArgument);
    return backend::open_audio_encoder(config);
}

Result<std::unique_ptr<AudioDecoder>> open_audio_decoder(const AudioDecoderConfig& config) {
    if (config.codec != Codec::Opus || !is_valid(config.format)) return fail(Errc::InvalidArgument);
    return backend::open_audio_decoder(config);
}

}  // namespace ratcast
