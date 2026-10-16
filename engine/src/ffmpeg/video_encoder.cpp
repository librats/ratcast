// Video encoding through FFmpeg's libavcodec: one implementation for every
// encoder, hardware or software, that takes pictures from CPU memory. What
// differs between them is how each is told to keep latency down, and whether
// it can change its bitrate in flight — the candidate table below.

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "ffmpeg/av.h"
#include "ratcast/engine/video_codec.h"
#include "ratcast/engine/video_convert.h"
#include "video/codec_backend.h"

#include "librats/util/logger.h"

namespace ratcast {
namespace {

/// Encoder buffer (VBV) in frames: how far one frame may overshoot its share of
/// the bitrate. Small keeps frame sizes even, which a live path needs; too
/// small starves keyframes of quality.
constexpr uint32_t kBufferFrames = 2;

/// gop_size when keyframes come only on request: longer than any session.
constexpr int kNoPeriodicKeyframes = 1 << 30;

struct Candidate {
    const char* name;
    Codec codec;
    bool hardware;
    /// Takes a new AVCodecContext::bit_rate between frames. The others are
    /// reopened, which costs a keyframe (and for SVT-AV1 some 30 ms at 1080p).
    bool dynamic_bitrate;
    void (*tune)(const VideoEncoderConfig&, AVCodecContext&, av::Dictionary&);
};

void tune_nvenc(const VideoEncoderConfig&, AVCodecContext&, av::Dictionary& options) {
    options.set("preset", "p4");
    options.set("tune", "ull");
    options.set("rc", "cbr");
    options.set("zerolatency", "1");
    options.set("delay", "0");
    options.set("forced-idr", "1");
}

void tune_qsv(const VideoEncoderConfig&, AVCodecContext&, av::Dictionary& options) {
    options.set("preset", "veryfast");
    options.set("async_depth", "1");
    options.set("low_delay_brc", "1");
    options.set("forced_idr", "1");
}

void tune_amf(const VideoEncoderConfig&, AVCodecContext&, av::Dictionary& options) {
    options.set("usage", "ultralowlatency");
    options.set("rc", "cbr");
    options.set("quality", "speed");
}

void tune_videotoolbox(const VideoEncoderConfig&, AVCodecContext&, av::Dictionary& options) {
    options.set("realtime", "1");
    options.set("prio_speed", "1");
}

void tune_svtav1(const VideoEncoderConfig& config, AVCodecContext& context,
                 av::Dictionary& options) {
    // SVT-AV1 runs low delay (pred-struct=1) only in CBR (rc=2), and takes a
    // maximum rate only in CRF mode.
    context.rc_max_rate = 0;
    const uint64_t pixel_rate = uint64_t{config.width} * config.height * config.frame_rate_num /
                                std::max<uint32_t>(config.frame_rate_den, 1);
    options.set("preset", pixel_rate <= uint64_t{1280} * 720 * 60 ? "10" : "11");

    // Its buffer levels are in milliseconds, each in [20, 10000] and below the
    // buffer size: at 60 fps and more, two frames are too short a buffer.
    constexpr int64_t kMinBufferMs = 25;
    const int64_t bitrate = std::max<int64_t>(context.bit_rate, 1);
    int64_t buffer_ms = int64_t{context.rc_buffer_size} * 1000 / bitrate;
    if (buffer_ms < kMinBufferMs) {
        buffer_ms = kMinBufferMs;
        context.rc_buffer_size = static_cast<int>(bitrate * kMinBufferMs / 1000);
    }
    const std::string level = std::to_string(std::clamp<int64_t>(buffer_ms / 2, 20, buffer_ms - 1));
    options.set("svtav1-params",
                "pred-struct=1:rc=2:buf-initial-sz=" + level + ":buf-optimal-sz=" + level);
}

void tune_aom(const VideoEncoderConfig&, AVCodecContext& context, av::Dictionary& options) {
    context.rc_min_rate = context.bit_rate;  // min = max = target: CBR
    options.set("usage", "realtime");
    options.set("cpu-used", "8");
    options.set("lag-in-frames", "0");
    options.set("row-mt", "1");
}

void tune_openh264(const VideoEncoderConfig&, AVCodecContext&, av::Dictionary& options) {
    options.set("profile", "constrained_baseline");
    options.set("rc_mode", "bitrate");
    options.set("allow_skip_frames", "0");
}

// Best first within each codec. VAAPI is missing on purpose: it takes only
// pictures already in GPU memory, which arrive with DMA-BUF capture.
constexpr Candidate kCandidates[] = {
    {"av1_nvenc", Codec::AV1, true, true, tune_nvenc},
    {"av1_qsv", Codec::AV1, true, true, tune_qsv},
    {"av1_amf", Codec::AV1, true, false, tune_amf},
    {"libsvtav1", Codec::AV1, false, false, tune_svtav1},
    {"libaom-av1", Codec::AV1, false, false, tune_aom},
    {"hevc_nvenc", Codec::H265, true, true, tune_nvenc},
    {"hevc_qsv", Codec::H265, true, true, tune_qsv},
    {"hevc_amf", Codec::H265, true, false, tune_amf},
    {"hevc_videotoolbox", Codec::H265, true, false, tune_videotoolbox},
    {"h264_nvenc", Codec::H264, true, true, tune_nvenc},
    {"h264_qsv", Codec::H264, true, true, tune_qsv},
    {"h264_amf", Codec::H264, true, false, tune_amf},
    {"h264_videotoolbox", Codec::H264, true, false, tune_videotoolbox},
    {"libopenh264", Codec::H264, false, false, tune_openh264},
};

/// The pixel formats `codec` takes, terminated by AV_PIX_FMT_NONE.
const AVPixelFormat* supported_formats(const AVCodec& codec) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* formats = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, &codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &formats,
                                     &count) < 0) {
        return nullptr;
    }
    return static_cast<const AVPixelFormat*>(formats);
#else
    return codec.pix_fmts;
#endif
}

/// NV12 if the encoder takes it — hardware encoders work in it natively —
/// otherwise planar 4:2:0.
std::optional<PixelFormat> choose_input_format(const AVCodec& codec) {
    const AVPixelFormat* formats = supported_formats(codec);
    if (!formats) return PixelFormat::I420;  // "anything": the safe common choice
    const auto takes = [formats](PixelFormat format) {
        for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
            if (*f == av::pixel_format(format)) return true;
        }
        return false;
    };
    for (PixelFormat preferred : {PixelFormat::NV12, PixelFormat::I420}) {
        if (takes(preferred)) return preferred;
    }
    return std::nullopt;
}

class FfmpegVideoEncoder final : public VideoEncoder {
public:
    FfmpegVideoEncoder(const Candidate& candidate, const AVCodec& codec,
                       const VideoEncoderConfig& config, PixelFormat input_format)
        : candidate_(candidate),
          codec_(codec),
          config_(config),
          info_{candidate.name, candidate.codec, candidate.hardware, input_format},
          frame_(av_frame_alloc()),
          packet_(av_packet_alloc()) {}

    ~FfmpegVideoEncoder() override {
        // Say the stream ended: SVT-AV1 complains when closed in mid-stream.
        if (context_) avcodec_send_frame(context_.get(), nullptr);
    }

    /// Open the codec for the current config.
    Result<void> open() {
        av::CodecContextPtr context(avcodec_alloc_context3(&codec_));
        if (!context || !frame_ || !packet_) return fail(Errc::Internal);

        context->width = static_cast<int>(config_.width);
        context->height = static_cast<int>(config_.height);
        context->pix_fmt = av::pixel_format(info_.input_format);
        context->time_base = av::kMicroseconds;
        context->framerate = AVRational{static_cast<int>(config_.frame_rate_num),
                                        static_cast<int>(config_.frame_rate_den)};
        av::set_color_space(*context, config_.color_space);
        context->max_b_frames = 0;
        context->gop_size = config_.keyframe_interval > 0
                                ? static_cast<int>(config_.keyframe_interval)
                                : kNoPeriodicKeyframes;
        apply_bitrate(*context);

        av::Dictionary options;
        candidate_.tune(config_, *context, options);
        const int error = avcodec_open2(context.get(), &codec_, options.get());
        if (error < 0) {
            LOG_DEBUG("engine",
                      "encoder " << info_.name << " cannot open: " << av::describe(error));
            return fail(error == AVERROR(EINVAL) ? Errc::InvalidArgument : Errc::Unavailable);
        }
        context_ = std::move(context);
        return {};
    }

    const VideoEncoderInfo& info() const noexcept override { return info_; }

    Result<void> encode(const VideoFrame& picture, bool keyframe, Sink sink) override {
        if (picture.format() == PixelFormat::BGRA || picture.width() != config_.width ||
            picture.height() != config_.height || picture.color_space() != config_.color_space) {
            return fail(Errc::InvalidArgument);
        }
        if (picture.format() != info_.input_format) {  // the other 4:2:0 layout
            Result<VideoFrame> converted = convert_frame(picture, info_.input_format);
            if (!converted) return fail(converted.error());
            return encode(*converted, keyframe, sink);
        }
        if (reopen_) {
            // Whatever the old encoder still holds belongs before this picture.
            if (Result<void> flushed = flush(sink); !flushed) return flushed;
            if (Result<void> opened = open(); !opened) return opened;
            reopen_ = false;
            keyframe = true;
        }

        if (Result<void> wrapped = wrap(picture, keyframe); !wrapped) return wrapped;
        const int error = avcodec_send_frame(context_.get(), frame_.get());
        av_frame_unref(frame_.get());
        if (error < 0) {
            LOG_WARN("engine", info_.name << " refused a picture: " << av::describe(error));
            return fail(av::errc_of(error));
        }
        return drain(sink);
    }

    void set_bitrate(uint32_t bits_per_second) override {
        if (bits_per_second == 0 || bits_per_second == config_.bitrate) return;
        config_.bitrate = bits_per_second;
        if (candidate_.dynamic_bitrate) {
            apply_bitrate(*context_);
        } else {
            reopen_ = true;
        }
    }

    // Not every encoder takes pictures again after being drained, so the next
    // picture starts a fresh one, with a keyframe.
    Result<void> flush(Sink sink) override {
        reopen_ = true;
        const int error = avcodec_send_frame(context_.get(), nullptr);
        if (error < 0 && error != AVERROR_EOF) return fail(av::errc_of(error));
        return drain(sink);
    }

private:
    void apply_bitrate(AVCodecContext& context) const {
        const int64_t bitrate = config_.bitrate;
        context.bit_rate = bitrate;
        context.rc_max_rate = bitrate;
        context.rc_buffer_size = static_cast<int>(std::min<int64_t>(
            bitrate * kBufferFrames * config_.frame_rate_den / config_.frame_rate_num,
            std::numeric_limits<int>::max()));
    }

    // Point frame_ at the picture's planes. The frame's buffer holds a copy of
    // the VideoFrame — not of its pixels — so an encoder that keeps the
    // picture keeps its memory alive, and FFmpeg never copies it to be sure.
    Result<void> wrap(const VideoFrame& picture, bool keyframe) {
        auto* keep = new VideoFrame(picture);
        frame_->buf[0] = av_buffer_create(
            const_cast<uint8_t*>(picture.plane(0).data), 1,
            [](void* opaque, uint8_t*) { delete static_cast<VideoFrame*>(opaque); }, keep,
            AV_BUFFER_FLAG_READONLY);
        if (!frame_->buf[0]) {
            delete keep;
            return fail(Errc::Internal);
        }
        frame_->format = context_->pix_fmt;
        frame_->width = context_->width;
        frame_->height = context_->height;
        frame_->color_primaries = context_->color_primaries;
        frame_->color_trc = context_->color_trc;
        frame_->colorspace = context_->colorspace;
        frame_->color_range = context_->color_range;
        for (size_t i = 0; i < picture.plane_count(); ++i) {
            frame_->data[i] = const_cast<uint8_t*>(picture.plane(i).data);
            frame_->linesize[i] = static_cast<int>(picture.plane(i).stride);
        }

        // Encoders insist on increasing timestamps; two pictures stamped alike
        // are a microsecond apart.
        int64_t pts = picture.capture_time().time_since_epoch().count();
        if (last_pts_ && pts <= *last_pts_) pts = *last_pts_ + 1;
        last_pts_ = pts;
        frame_->pts = pts;
        frame_->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        return {};
    }

    Result<void> drain(Sink sink) {
        for (;;) {
            const int error = avcodec_receive_packet(context_.get(), packet_.get());
            if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) return {};
            if (error < 0) {
                LOG_WARN("engine", info_.name << " failed: " << av::describe(error));
                return fail(av::errc_of(error));
            }

            EncodedVideo out;
            out.info.number = next_number_++;
            out.info.keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
            out.info.capture_time = Timestamp{Duration{packet_->pts}};
            // One chain from keyframe to keyframe: each frame needs the one
            // before (and through it everything back to the keyframe).
            if (!out.info.keyframe && out.info.number > 0) {
                out.info.references = {out.info.number - 1};
            }
            const auto size = static_cast<size_t>(packet_->size);
            out.payload = allocate_frame_payload(size);
            out.payload.append(std::span(packet_->data, size));
            av_packet_unref(packet_.get());
            sink(std::move(out));
        }
    }

    const Candidate& candidate_;
    const AVCodec& codec_;
    VideoEncoderConfig config_;
    VideoEncoderInfo info_;

    av::CodecContextPtr context_;
    av::FramePtr frame_;
    av::PacketPtr packet_;
    bool reopen_ = false;
    std::optional<int64_t> last_pts_;
    FrameNumber next_number_ = 0;
};

}  // namespace

namespace backend {

std::vector<std::string> video_encoder_candidates(Codec codec) {
    std::vector<std::string> names;
    for (const Candidate& candidate : kCandidates) {
        if (candidate.codec == codec && avcodec_find_encoder_by_name(candidate.name)) {
            names.emplace_back(candidate.name);
        }
    }
    return names;
}

Result<std::unique_ptr<VideoEncoder>> open_video_encoder(const VideoEncoderConfig& config) {
    for (const Candidate& candidate : kCandidates) {
        if (candidate.codec != config.codec) continue;
        if (!config.encoder.empty() && config.encoder != candidate.name) continue;
        if (candidate.hardware ? !config.allow_hardware : !config.allow_software) continue;

        const AVCodec* codec = avcodec_find_encoder_by_name(candidate.name);
        if (!codec) continue;
        const std::optional<PixelFormat> format = choose_input_format(*codec);
        if (!format) continue;

        auto encoder = std::make_unique<FfmpegVideoEncoder>(candidate, *codec, config, *format);
        if (encoder->open()) {
            LOG_INFO("engine", "video encoder: " << candidate.name << " " << config.width << "x"
                                                 << config.height << " at " << config.bitrate
                                                 << " bit/s");
            return std::unique_ptr<VideoEncoder>(std::move(encoder));
        }
    }
    return fail(Errc::Unavailable);
}

}  // namespace backend
}  // namespace ratcast
