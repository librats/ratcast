// Audio decoding through libopus, and making up what never arrived: Opus'
// own concealment, which continues the sound and fades it out (or makes
// comfort noise after DTX), and recovery from the in-band FEC of the frame
// after a loss.

#include <opus.h>

#include <algorithm>
#include <memory>

#include "audio/audio_backend.h"
#include "ratcast/engine/audio_codec.h"

#include "librats/util/logger.h"

namespace ratcast {
namespace {

struct DecoderDeleter {
    void operator()(OpusDecoder* decoder) const noexcept { opus_decoder_destroy(decoder); }
};

bool has_fec(const EncodedFrame& frame) {
#ifdef RATCAST_OPUS_HAS_LBRR
    return opus_packet_has_lbrr(frame.payload.data(),
                                static_cast<opus_int32>(frame.payload.size())) > 0;
#else
    (void)frame;
    return false;  // libopus before 1.5 cannot tell; counted as concealed
#endif
}

class OpusAudioDecoder final : public AudioDecoder {
public:
    OpusAudioDecoder(std::unique_ptr<OpusDecoder, DecoderDeleter> decoder, AudioFormat format)
        : decoder_(std::move(decoder)), info_{opus_get_version_string(), format} {}

    const AudioDecoderInfo& info() const noexcept override { return info_; }

    Result<AudioFrame> decode(const EncodedFrame& frame) override {
        // An empty frame is nothing Opus makes, and libopus may not be handed one.
        if (frame.payload.empty()) return fail(Errc::Malformed);
        const auto* data = frame.payload.data();
        const auto size = static_cast<opus_int32>(frame.payload.size());
        const int length = opus_decoder_get_nb_samples(decoder_.get(), data, size);
        if (length <= 0) return fail(Errc::Malformed);
        MutableAudioFrame out = MutableAudioFrame::allocate(info_.format, length);
        const int decoded =
            opus_decode_float(decoder_.get(), data, size, out.samples().data(), length, 0);
        if (decoded < 0) {
            LOG_DEBUG("engine", "frame " << frame.info.number
                                         << " failed to decode: " << opus_strerror(decoded));
            return fail(Errc::Malformed);
        }
        stats_.decoded += audio_duration(length, info_.format.sample_rate);
        return std::move(out).freeze(frame.info.capture_time);
    }

    Result<AudioFrame> conceal(Duration missing, Timestamp capture_time,
                               const EncodedFrame* next) override {
        const uint32_t rate = info_.format.sample_rate;
        const size_t quantum = rate / 400;  // 2.5 ms, the unit Opus decodes in
        const size_t length = audio_length(missing, rate) / quantum * quantum;
        if (length == 0) return AudioFrame{};

        // FEC in `next` stands for one frame of its own length: the last one
        // before it.
        size_t from_fec = 0;
        if (next && !next->payload.empty()) {
            const int n = opus_packet_get_nb_samples(next->payload.data(),
                                                     static_cast<opus_int32>(next->payload.size()),
                                                     static_cast<opus_int32>(rate));
            if (n > 0 && static_cast<size_t>(n) <= length) from_fec = static_cast<size_t>(n);
        }

        MutableAudioFrame out = MutableAudioFrame::allocate(info_.format, length);
        float* pcm = out.samples().data();
        const size_t channels = info_.format.channels;
        const size_t max_chunk = rate * 120 / 1000;  // the most one call decodes
        for (size_t done = 0; done < length - from_fec;) {
            const size_t chunk = std::min(max_chunk, length - from_fec - done);
            if (opus_decode_float(decoder_.get(), nullptr, 0, pcm + done * channels,
                                  static_cast<int>(chunk), 0) < 0)
                return fail(Errc::Internal);
            done += chunk;
        }
        if (from_fec > 0) {
            if (opus_decode_float(decoder_.get(), next->payload.data(),
                                  static_cast<opus_int32>(next->payload.size()),
                                  pcm + (length - from_fec) * channels, static_cast<int>(from_fec),
                                  1) < 0)
                return fail(Errc::Malformed);
        }

        const size_t recovered = from_fec > 0 && has_fec(*next) ? from_fec : 0;
        stats_.recovered += audio_duration(recovered, rate);
        stats_.concealed += audio_duration(length - recovered, rate);
        return std::move(out).freeze(capture_time);
    }

    AudioDecoderStats stats() const noexcept override { return stats_; }

private:
    std::unique_ptr<OpusDecoder, DecoderDeleter> decoder_;
    const AudioDecoderInfo info_;
    AudioDecoderStats stats_;
};

}  // namespace

namespace backend {

Result<std::unique_ptr<AudioDecoder>> open_audio_decoder(const AudioDecoderConfig& config) {
    int error = OPUS_OK;
    std::unique_ptr<OpusDecoder, DecoderDeleter> decoder(opus_decoder_create(
        static_cast<opus_int32>(config.format.sample_rate), config.format.channels, &error));
    if (error != OPUS_OK || !decoder) {
        LOG_ERROR("engine", "opus_decoder_create: " << opus_strerror(error));
        return fail(Errc::Internal);
    }
    return std::unique_ptr<AudioDecoder>(new OpusAudioDecoder(std::move(decoder), config.format));
}

}  // namespace backend
}  // namespace ratcast
