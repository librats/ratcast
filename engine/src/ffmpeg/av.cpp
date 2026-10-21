#include "ffmpeg/av.h"

#include "video/codec_backend.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <span>

extern "C" {
#include <libavutil/log.h>
}

namespace ratcast::av {

std::string describe(int error) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof text);
    return text;
}

Errc errc_of(int error) noexcept {
    switch (error) {
        case AVERROR_INVALIDDATA: return Errc::Malformed;
        case AVERROR(EINVAL): return Errc::InvalidArgument;
        case AVERROR(ENOSYS):
        case AVERROR_ENCODER_NOT_FOUND:
        case AVERROR_DECODER_NOT_FOUND: return Errc::Unavailable;
        case AVERROR_PATCHWELCOME: return Errc::Unsupported;
        default: return Errc::Internal;
    }
}

AVCodecID codec_id(Codec codec) noexcept {
    switch (codec) {
        case Codec::H264: return AV_CODEC_ID_H264;
        case Codec::H265: return AV_CODEC_ID_HEVC;
        case Codec::AV1: return AV_CODEC_ID_AV1;
        case Codec::Opus: return AV_CODEC_ID_OPUS;
        case Codec::MpegTs: break;  // a container: nothing to code
    }
    return AV_CODEC_ID_NONE;
}

const AVCodec* find_decoder(AVCodecID id) noexcept {
    if (id == AV_CODEC_ID_AV1) return avcodec_find_decoder_by_name("libdav1d");
    return avcodec_find_decoder(id);
}

AVPixelFormat pixel_format(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::I420: return AV_PIX_FMT_YUV420P;
        case PixelFormat::NV12: return AV_PIX_FMT_NV12;
        case PixelFormat::BGRA: return AV_PIX_FMT_BGRA;
    }
    return AV_PIX_FMT_NONE;
}

std::optional<PixelFormat> pixel_format(AVPixelFormat format) noexcept {
    switch (format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P: return PixelFormat::I420;
        case AV_PIX_FMT_NV12: return PixelFormat::NV12;
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_BGR0: return PixelFormat::BGRA;
        default: return std::nullopt;
    }
}

void set_color_space(AVCodecContext& context, ColorSpace color_space) noexcept {
    context.color_primaries = AVCOL_PRI_BT709;
    context.color_trc = AVCOL_TRC_BT709;
    context.colorspace =
        color_space.matrix == ColorMatrix::BT601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
    context.color_range =
        color_space.range == ColorRange::Full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

ColorSpace color_space(const AVFrame& frame) noexcept {
    ColorSpace color_space;
    if (frame.colorspace == AVCOL_SPC_SMPTE170M || frame.colorspace == AVCOL_SPC_BT470BG) {
        color_space.matrix = ColorMatrix::BT601;
    }
    if (frame.color_range == AVCOL_RANGE_JPEG || frame.format == AV_PIX_FMT_YUVJ420P) {
        color_space.range = ColorRange::Full;
    }
    return color_space;
}

std::optional<VideoFrame> take_picture(AVFrame& frame, uint32_t width, uint32_t height,
                                       Timestamp capture_time) {
    assert(width <= static_cast<uint32_t>(frame.width) &&
           height <= static_cast<uint32_t>(frame.height));
    const std::optional<PixelFormat> format =
        pixel_format(static_cast<AVPixelFormat>(frame.format));
    if (!format) return std::nullopt;
    std::shared_ptr<AVFrame> owner(av_frame_alloc(), FrameDeleter{});
    if (!owner) return std::nullopt;
    av_frame_move_ref(owner.get(), &frame);

    VideoFrame::Plane planes[VideoFrame::kMaxPlanes];
    for (size_t i = 0; i < plane_count(*format); ++i) {
        planes[i] = {owner->data[i], static_cast<size_t>(owner->linesize[i])};
    }
    const ColorSpace colors = color_space(*owner);
    return VideoFrame(*format, width, height, std::span(planes, plane_count(*format)),
                      std::move(owner), capture_time, colors);
}

}  // namespace ratcast::av

namespace ratcast::backend {

void set_codec_logging(bool verbose) {
    av_log_set_level(verbose ? AV_LOG_INFO : AV_LOG_FATAL);
    // SVT-AV1 has a log of its own, set from the environment: 1 is errors.
    const char* level = verbose ? "3" : "1";
#ifdef _WIN32
    _putenv_s("SVT_LOG", level);
#else
    setenv("SVT_LOG", level, 1);
#endif
}

}  // namespace ratcast::backend
