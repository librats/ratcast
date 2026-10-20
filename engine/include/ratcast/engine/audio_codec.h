#pragma once

/**
 * @file audio_codec.h
 * @brief Encoding sound into frames and decoding them back, losses included.
 *
 * The codec is Opus (libopus), in its two uses: voice — mono, a few tens of
 * kbit/s, silence sent as nothing (DTX) and each frame carrying a coarse copy
 * of the one before it (in-band FEC) — and music, stereo at a high bitrate.
 *
 * Every frame decodes on its own, so each is a keyframe without references;
 * what a lost frame costs is the sound it carried, which the decoder makes up.
 * A receiver plays frames by their capture times. Frame numbers have no gaps
 * on the sender's side, as the session needs, while silence the encoder left
 * out shows as a jump in capture times: a gap in numbers is a loss, a gap in
 * time with none in numbers is silence.
 *
 * Encoders and decoders are not thread-safe: each belongs to one thread.
 */

#include <cstdint>
#include <memory>
#include <string>

#include "ratcast/core/buffer.h"
#include "ratcast/core/function_ref.h"
#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/audio_frame.h"
#include "ratcast/stream/frame.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast {

// ── Encoding ────────────────────────────────────────────────────────────────

/// What the sound is, which decides how Opus codes it.
enum class AudioContent : uint8_t {
    Voice,  ///< speech: tuned for intelligibility, DTX and FEC pay off
    Music,  ///< anything else: a film's or a game's sound, full band
};

struct AudioEncoderConfig {
    Codec codec = Codec::Opus;
    /// What encode() takes: 8, 12, 16, 24 or 48 kHz, one or two channels.
    AudioFormat format{48'000, 1};
    AudioContent content = AudioContent::Voice;
    uint32_t bitrate = 32'000;  ///< bits per second, 6 to 510 kbit/s
    /// The sound in one frame: 10, 20, 40 or 60 ms. Shorter is less latency
    /// and more packets.
    Duration frame_duration = std::chrono::milliseconds(20);
    /// Send nothing in silence but a frame every 400 ms that keeps the far
    /// side's comfort noise alive.
    bool dtx = false;
    /// Packets expected lost, in percent. Above 0 every frame carries a copy of
    /// the one before it at a lower rate (in-band FEC), sized for this loss —
    /// only in the modes Opus codes speech in (SILK, hybrid), which a voice at
    /// up to ~40 kbit/s is.
    uint8_t expected_loss = 0;
};

struct AudioEncoderInfo {
    std::string name;  ///< the library and its version, e.g. "libopus 1.6.1"
    Codec codec = Codec::Opus;
    AudioFormat format;
    Duration frame_duration{};
};

/// A frame out of an encoder, ready for MediaSession::send_frame(): numbered
/// from 0, a keyframe without references, stamped with the capture time of
/// its first sample, and a payload with headroom for the wire header.
struct EncodedAudio {
    FrameInfo info;
    MutableBuffer payload;
};

class AudioEncoder {
public:
    using Sink = FunctionRef<void(EncodedAudio&&)>;

    virtual ~AudioEncoder() = default;

    virtual const AudioEncoderInfo& info() const noexcept = 0;

    /// Encode `samples`, of the configured format and any length. Every frame
    /// they complete is handed to `sink` before this returns; what is left
    /// over waits for the next block. A frame DTX leaves out is not handed on
    /// at all. Fails with InvalidArgument for samples of another format.
    virtual Result<void> encode(const AudioFrame& samples, Sink sink) = 0;

    /// Aim at `bits_per_second` from the next frame on.
    virtual void set_bitrate(uint32_t bits_per_second) = 0;

    /// Size FEC for `percent` of packets lost from the next frame on; 0 turns
    /// it off.
    virtual void set_expected_loss(uint8_t percent) = 0;
};

/// An encoder for `config`. Fails with InvalidArgument for a config Opus does
/// not take (rate, channels, frame duration, bitrate), Unavailable in a build
/// without libopus.
Result<std::unique_ptr<AudioEncoder>> open_audio_encoder(const AudioEncoderConfig& config);

// ── Decoding ────────────────────────────────────────────────────────────────

struct AudioDecoderConfig {
    Codec codec = Codec::Opus;
    /// What the decoder gives out, whatever the sender encoded: 8, 12, 16, 24
    /// or 48 kHz, one or two channels.
    AudioFormat format{48'000, 1};
    /// Out-of-band configuration from the stream description, if any.
    Buffer codec_config;
};

struct AudioDecoderInfo {
    std::string name;  ///< the library and its version, e.g. "libopus 1.6.1"
    AudioFormat format;
};

/// How much sound a decoder gave out, by where it came from.
struct AudioDecoderStats {
    Duration decoded{};    ///< from frames that arrived
    Duration recovered{};  ///< from the FEC of the frame after a lost one
    Duration concealed{};  ///< made up: after a loss without FEC, or in DTX's silence
};

class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    virtual const AudioDecoderInfo& info() const noexcept = 0;

    /// Decode `frame` into the configured format, carrying the frame's capture
    /// time. Fails with Malformed for a frame the decoder cannot make sense of.
    virtual Result<AudioFrame> decode(const EncodedFrame& frame) = 0;

    /// Make up `missing` sound that never arrived — lost, or left out by DTX —
    /// starting at `capture_time`; `missing` is cut to whole 2.5 ms. With
    /// `next`, the frame that arrived after the gap, its last frame's worth is
    /// taken from the FEC in `next` where there is some; the rest, and all of
    /// it without `next`, is concealment: a continuation that fades out, or
    /// comfort noise in DTX. Decode `next` itself after this.
    virtual Result<AudioFrame> conceal(Duration missing, Timestamp capture_time,
                                       const EncodedFrame* next = nullptr) = 0;

    virtual AudioDecoderStats stats() const noexcept = 0;
};

/// A decoder for `config`. Fails with InvalidArgument for a format the codec
/// cannot give out or a codec that is not audio, Unavailable in a build
/// without libopus.
Result<std::unique_ptr<AudioDecoder>> open_audio_decoder(const AudioDecoderConfig& config);

}  // namespace ratcast
