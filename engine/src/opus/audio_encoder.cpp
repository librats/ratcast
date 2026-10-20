// Audio encoding through libopus. Blocks of any length go in and are cut into
// frames of the configured duration; a frame is encoded straight from the
// block when it lies whole inside one, and gathered in a buffer of its own
// when it spans two.

#include <opus.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "audio/audio_backend.h"
#include "ratcast/engine/audio_codec.h"

#include "librats/util/logger.h"

namespace ratcast {
namespace {

// Opus' recommended output buffer: room for 60 ms at 510 kbit/s.
constexpr size_t kMaxPacket = 4000;

// A DTX frame says only "silence goes on": one or two bytes, the TOC.
constexpr int kMaxDtxPacket = 2;

struct EncoderDeleter {
    void operator()(OpusEncoder* encoder) const noexcept { opus_encoder_destroy(encoder); }
};

// In-band FEC, sized for `percent` of packets lost; off at 0.
void set_loss(OpusEncoder* encoder, uint8_t percent) {
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(percent > 0 ? 1 : 0));
    opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(percent));
}

class OpusAudioEncoder final : public AudioEncoder {
public:
    OpusAudioEncoder(std::unique_ptr<OpusEncoder, EncoderDeleter> encoder,
                     const AudioEncoderConfig& config)
        : encoder_(std::move(encoder)),
          info_{opus_get_version_string(), Codec::Opus, config.format, config.frame_duration},
          frame_length_(audio_length(config.frame_duration, config.format.sample_rate)),
          dtx_(config.dtx),
          pending_(frame_length_ * config.format.channels) {}

    const AudioEncoderInfo& info() const noexcept override { return info_; }

    Result<void> encode(const AudioFrame& samples, Sink sink) override {
        if (samples.format() != info_.format) return fail(Errc::InvalidArgument);
        const size_t channels = info_.format.channels;
        const std::span<const float> in = samples.samples();
        size_t offset = 0;  // samples per channel taken from `in`
        while (offset < samples.length()) {
            const Timestamp at =
                samples.capture_time() + audio_duration(offset, info_.format.sample_rate);
            const size_t left = samples.length() - offset;
            if (pending_length_ == 0 && left >= frame_length_) {
                if (Result<void> r = encode_frame(&in[offset * channels], at, sink); !r) return r;
                offset += frame_length_;
                continue;
            }
            if (pending_length_ == 0) pending_time_ = at;
            const size_t take = std::min(frame_length_ - pending_length_, left);
            std::memcpy(&pending_[pending_length_ * channels], &in[offset * channels],
                        take * channels * sizeof(float));
            pending_length_ += take;
            offset += take;
            if (pending_length_ == frame_length_) {
                pending_length_ = 0;
                if (Result<void> r = encode_frame(pending_.data(), pending_time_, sink); !r)
                    return r;
            }
        }
        return {};
    }

    void set_bitrate(uint32_t bits_per_second) override {
        opus_encoder_ctl(encoder_.get(),
                         OPUS_SET_BITRATE(static_cast<opus_int32>(
                             std::clamp<uint32_t>(bits_per_second, 6'000, 510'000))));
    }

    void set_expected_loss(uint8_t percent) override {
        set_loss(encoder_.get(), std::min<uint8_t>(percent, 100));
    }

private:
    Result<void> encode_frame(const float* pcm, Timestamp capture_time, Sink sink) {
        MutableBuffer payload = allocate_frame_payload(kMaxPacket);
        const opus_int32 size =
            opus_encode_float(encoder_.get(), pcm, static_cast<int>(frame_length_),
                              payload.append(kMaxPacket), static_cast<opus_int32>(kMaxPacket));
        if (size < 0) {
            LOG_ERROR("engine", "opus_encode_float: " << opus_strerror(size));
            return fail(Errc::Internal);
        }
        // Of a run of DTX frames only the first goes out: it puts the far
        // side's decoder into comfort noise, and the rest would repeat it.
        const bool dtx_frame = dtx_ && size <= kMaxDtxPacket;
        if (dtx_frame && in_dtx_) return {};
        in_dtx_ = dtx_frame;

        payload.resize(static_cast<size_t>(size));
        EncodedAudio frame;
        frame.info.number = next_number_++;
        frame.info.capture_time = capture_time;
        frame.info.keyframe = true;
        frame.payload = std::move(payload);
        sink(std::move(frame));
        return {};
    }

    std::unique_ptr<OpusEncoder, EncoderDeleter> encoder_;
    const AudioEncoderInfo info_;
    const size_t frame_length_;  ///< samples per channel in a frame
    const bool dtx_;
    bool in_dtx_ = false;
    FrameNumber next_number_ = 0;
    /// A frame begun in one block and finished in the next.
    std::vector<float> pending_;
    size_t pending_length_ = 0;
    Timestamp pending_time_{};
};

}  // namespace

namespace backend {

Result<std::unique_ptr<AudioEncoder>> open_audio_encoder(const AudioEncoderConfig& config) {
    const bool voice = config.content == AudioContent::Voice;
    int error = OPUS_OK;
    std::unique_ptr<OpusEncoder, EncoderDeleter> encoder(opus_encoder_create(
        static_cast<opus_int32>(config.format.sample_rate), config.format.channels,
        voice ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO, &error));
    if (error != OPUS_OK || !encoder) {
        LOG_ERROR("engine", "opus_encoder_create: " << opus_strerror(error));
        return fail(Errc::Internal);
    }
    opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(static_cast<opus_int32>(config.bitrate)));
    opus_encoder_ctl(encoder.get(), OPUS_SET_SIGNAL(voice ? OPUS_SIGNAL_VOICE : OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(encoder.get(), OPUS_SET_DTX(config.dtx ? 1 : 0));
    set_loss(encoder.get(), config.expected_loss);
    return std::unique_ptr<AudioEncoder>(new OpusAudioEncoder(std::move(encoder), config));
}

}  // namespace backend
}  // namespace ratcast
