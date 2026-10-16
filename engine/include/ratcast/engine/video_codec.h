#pragma once

/**
 * @file video_codec.h
 * @brief Encoding pictures into frames and decoding them back.
 *
 * Both sides are tuned for live use: no B-frames and no lookahead, so a frame
 * comes out of the encoder as soon as its picture went in, and out of the
 * decoder as soon as its frame did. Parameter sets (SPS/PPS, the AV1 sequence
 * header) travel inside every keyframe, so a keyframe decodes on its own.
 *
 * open_video_encoder() picks the best encoder there is: hardware first, then
 * software. Encoders under the GPL (x264, x265) are never picked, so ratcast
 * behaves the same on an LGPL build of FFmpeg as on a distribution's.
 *
 * Encoders and decoders are not thread-safe: each belongs to one thread.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ratcast/core/buffer.h"
#include "ratcast/core/function_ref.h"
#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"
#include "ratcast/stream/frame.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast {

// ── Encoding ────────────────────────────────────────────────────────────────

struct VideoEncoderConfig {
    Codec codec = Codec::AV1;
    uint32_t width = 0;
    uint32_t height = 0;
    /// The rate pictures come at, for rate control.
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;
    uint32_t bitrate = 4'000'000;  ///< bits per second
    /// The colour space of the pictures, signalled in the stream.
    ColorSpace color_space;
    /// Most frames between two keyframes; 0 for keyframes only when asked.
    uint32_t keyframe_interval = 0;
    /// Use this FFmpeg encoder ("libsvtav1", "h264_nvenc") instead of choosing.
    std::string encoder;
    bool allow_hardware = true;
    bool allow_software = true;
};

struct VideoEncoderInfo {
    std::string name;  ///< the FFmpeg encoder, e.g. "av1_nvenc"
    Codec codec = Codec::AV1;
    bool hardware = false;
    /// What the encoder works in. encode() takes the other 4:2:0 layout too,
    /// at the cost of converting it first.
    PixelFormat input_format = PixelFormat::NV12;
};

/// A frame out of an encoder, ready for MediaSession::send_frame(): numbered
/// from 0 in decode order, with its references, and a payload with headroom
/// for the wire header.
struct EncodedVideo {
    FrameInfo info;
    MutableBuffer payload;
};

class VideoEncoder {
public:
    using Sink = FunctionRef<void(EncodedVideo&&)>;

    virtual ~VideoEncoder() = default;

    virtual const VideoEncoderInfo& info() const noexcept = 0;

    /// Encode `picture` — of the configured size and colour space, in NV12 or
    /// I420 — as a keyframe if `keyframe`. Every frame the encoder completes is
    /// handed to `sink` before this returns.
    virtual Result<void> encode(const VideoFrame& picture, bool keyframe, Sink sink) = 0;

    /// Aim at `bits_per_second` from the next picture on. An encoder that
    /// cannot change its rate in flight starts over, with a keyframe.
    virtual void set_bitrate(uint32_t bits_per_second) = 0;

    /// Hand every frame still inside the encoder to `sink`. The next picture
    /// starts over: it becomes a keyframe, and numbering carries on.
    virtual Result<void> flush(Sink sink) = 0;
};

/// The best encoder `config` allows. Fails with Unavailable when none of the
/// candidates can be opened here, InvalidArgument for a config that makes no
/// sense (no size, no bitrate, an audio codec).
Result<std::unique_ptr<VideoEncoder>> open_video_encoder(const VideoEncoderConfig& config);

/// The encoders open_video_encoder() would try for `codec`, best first — those
/// this build of FFmpeg has, whether or not the hardware is there.
std::vector<std::string> video_encoder_candidates(Codec codec);

// ── Decoding ────────────────────────────────────────────────────────────────

struct VideoDecoderConfig {
    Codec codec = Codec::AV1;
    /// Out-of-band configuration from the stream description, if any.
    Buffer codec_config;
};

struct VideoDecoderInfo {
    std::string name;  ///< the FFmpeg decoder, e.g. "libdav1d"
};

class VideoDecoder {
public:
    using Sink = FunctionRef<void(VideoFrame&&)>;

    virtual ~VideoDecoder() = default;

    virtual const VideoDecoderInfo& info() const noexcept = 0;

    /// Decode `frame`. Every picture the decoder completes is handed to `sink`
    /// before this returns, carrying the frame's capture time and the colour
    /// space the stream signals. Fails with
    /// Malformed for a frame the decoder cannot make sense of; the decoder
    /// recovers at the next keyframe.
    virtual Result<void> decode(const EncodedFrame& frame, Sink sink) = 0;
};

/// A decoder for `config.codec`. Fails with Unavailable when this build of
/// FFmpeg has none.
Result<std::unique_ptr<VideoDecoder>> open_video_decoder(const VideoDecoderConfig& config);

// ── Logging ─────────────────────────────────────────────────────────────────

/// What the codec libraries print themselves: only fatal errors (the default
/// for tools; the engine reports what fails through its own log), or all they
/// have to say. Before opening codecs, as SVT-AV1 reads it once per encoder.
void set_codec_logging(bool verbose);

}  // namespace ratcast
