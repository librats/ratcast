#pragma once

/**
 * @file av.h
 * @brief What every FFmpeg-facing file needs: ownership, errors, formats.
 */

#include <memory>
#include <optional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast::av {

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};

using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

/// Options for avcodec_open2(), freed with it.
class Dictionary {
public:
    Dictionary() = default;
    Dictionary(const Dictionary&) = delete;
    Dictionary& operator=(const Dictionary&) = delete;
    ~Dictionary() { av_dict_free(&dict_); }

    void set(const char* key, const char* value) { av_dict_set(&dict_, key, value, 0); }
    void set(const char* key, const std::string& value) { set(key, value.c_str()); }
    AVDictionary** get() noexcept { return &dict_; }

private:
    AVDictionary* dict_ = nullptr;
};

/// FFmpeg's description of `error`, for the log.
std::string describe(int error);

/// The failure an FFmpeg error code amounts to.
Errc errc_of(int error) noexcept;

AVCodecID codec_id(Codec codec) noexcept;

AVPixelFormat pixel_format(PixelFormat format) noexcept;
std::optional<PixelFormat> pixel_format(AVPixelFormat format) noexcept;

/// Microseconds, the unit of every timestamp passed to and from FFmpeg.
inline constexpr AVRational kMicroseconds{1, 1'000'000};

}  // namespace ratcast::av
