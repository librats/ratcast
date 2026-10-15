// Video decoding through FFmpeg's libavcodec, set up for live input: slice
// threads only (frame threads hold back a frame per thread), no reordering
// delay, every picture out as soon as its frame went in.

#include <cstring>
#include <memory>
#include <span>
#include <utility>

#include "ffmpeg/av.h"
#include "ratcast/engine/video_codec.h"
#include "video/codec_backend.h"

#include "librats/util/logger.h"

namespace ratcast {
namespace {

/// Decoders worth trying for `codec`, best first. AV1 needs libdav1d: FFmpeg's
/// own AV1 decoder decodes only through a hardware accelerator.
std::span<const char* const> decoder_names(Codec codec) {
    static constexpr const char* kAv1[] = {"libdav1d"};
    static constexpr const char* kH264[] = {"h264"};
    static constexpr const char* kH265[] = {"hevc"};
    switch (codec) {
        case Codec::AV1: return kAv1;
        case Codec::H264: return kH264;
        case Codec::H265: return kH265;
        case Codec::Opus: break;
    }
    return {};
}

class FfmpegVideoDecoder final : public VideoDecoder {
public:
    FfmpegVideoDecoder(av::CodecContextPtr context, std::string name)
        : context_(std::move(context)),
          info_{std::move(name)},
          packet_(av_packet_alloc()),
          frame_(av_frame_alloc()) {}

    bool valid() const noexcept { return packet_ && frame_; }

    const VideoDecoderInfo& info() const noexcept override { return info_; }

    Result<void> decode(const EncodedFrame& frame, Sink sink) override {
        // Copied into a packet of FFmpeg's own, which brings the zeroed
        // padding its bitstream readers may read into.
        const size_t size = frame.payload.size();
        if (av_new_packet(packet_.get(), static_cast<int>(size)) < 0) return fail(Errc::Internal);
        if (size > 0) std::memcpy(packet_->data, frame.payload.data(), size);
        packet_->pts = frame.info.capture_time.time_since_epoch().count();
        if (frame.info.keyframe) packet_->flags |= AV_PKT_FLAG_KEY;

        const int error = avcodec_send_packet(context_.get(), packet_.get());
        av_packet_unref(packet_.get());
        if (error < 0) {
            LOG_DEBUG("engine", info_.name << " refused frame " << frame.info.number << ": "
                                           << av::describe(error));
            return fail(av::errc_of(error));
        }
        return drain(sink);
    }

private:
    Result<void> drain(Sink sink) {
        for (;;) {
            const int error = avcodec_receive_frame(context_.get(), frame_.get());
            if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) return {};
            if (error < 0) return fail(av::errc_of(error));

            const std::optional<PixelFormat> format =
                av::pixel_format(static_cast<AVPixelFormat>(frame_->format));
            if (!format) {
                LOG_WARN("engine", info_.name << " produced pixel format " << frame_->format
                                              << ", which the engine does not handle");
                av_frame_unref(frame_.get());
                return fail(Errc::Unsupported);
            }

            // The picture stays in FFmpeg's buffers; the VideoFrame holds a
            // reference to them.
            std::shared_ptr<AVFrame> owner(av_frame_alloc(), av::FrameDeleter{});
            if (!owner) return fail(Errc::Internal);
            av_frame_move_ref(owner.get(), frame_.get());

            VideoFrame::Plane planes[VideoFrame::kMaxPlanes];
            for (size_t i = 0; i < plane_count(*format); ++i) {
                planes[i] = {owner->data[i], static_cast<size_t>(owner->linesize[i])};
            }
            const int64_t pts = owner->best_effort_timestamp != AV_NOPTS_VALUE
                                    ? owner->best_effort_timestamp
                                    : owner->pts;
            VideoFrame picture(
                *format, static_cast<uint32_t>(owner->width), static_cast<uint32_t>(owner->height),
                std::span(planes, plane_count(*format)), owner, Timestamp{Duration{pts}});
            sink(std::move(picture));
        }
    }

    av::CodecContextPtr context_;
    VideoDecoderInfo info_;
    av::PacketPtr packet_;
    av::FramePtr frame_;
};

Result<std::unique_ptr<VideoDecoder>> open_decoder(const AVCodec& codec,
                                                   const VideoDecoderConfig& config) {
    av::CodecContextPtr context(avcodec_alloc_context3(&codec));
    if (!context) return fail(Errc::Internal);
    context->pkt_timebase = av::kMicroseconds;
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->thread_type = FF_THREAD_SLICE;
    context->thread_count = 0;

    if (!config.codec_config.empty()) {
        const size_t size = config.codec_config.size();
        context->extradata = static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!context->extradata) return fail(Errc::Internal);
        std::memcpy(context->extradata, config.codec_config.data(), size);
        context->extradata_size = static_cast<int>(size);
    }

    av::Dictionary options;
    options.set("max_frame_delay", "1");  // libdav1d: no frames held back for its threads
    const int error = avcodec_open2(context.get(), &codec, options.get());
    if (error < 0) {
        LOG_DEBUG("engine", "decoder " << codec.name << " cannot open: " << av::describe(error));
        return fail(Errc::Unavailable);
    }

    auto decoder = std::make_unique<FfmpegVideoDecoder>(std::move(context), codec.name);
    if (!decoder->valid()) return fail(Errc::Internal);
    return std::unique_ptr<VideoDecoder>(std::move(decoder));
}

}  // namespace

namespace backend {

Result<std::unique_ptr<VideoDecoder>> open_video_decoder(const VideoDecoderConfig& config) {
    for (const char* name : decoder_names(config.codec)) {
        const AVCodec* codec = avcodec_find_decoder_by_name(name);
        if (!codec) continue;
        Result<std::unique_ptr<VideoDecoder>> decoder = open_decoder(*codec, config);
        if (decoder) {
            LOG_INFO("engine", "video decoder: " << name);
            return decoder;
        }
    }
    return fail(Errc::Unavailable);
}

}  // namespace backend
}  // namespace ratcast
