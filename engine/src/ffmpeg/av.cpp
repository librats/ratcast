#include "ffmpeg/av.h"

#include <cerrno>

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
    }
    return AV_CODEC_ID_NONE;
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

}  // namespace ratcast::av
