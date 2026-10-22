// Video encoding through FFmpeg's libavcodec: one implementation for every
// encoder, hardware or software. What differs between them is how each is
// told to keep latency down, and whether it can change its bitrate in flight —
// the candidate table below. Pictures come from memory, or — to an encoder
// opened on a frames context of their GPU's (hw_frames.h) — from GPU memory
// without passing through the CPU.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include "ffmpeg/av.h"
#include "ffmpeg/hw_frames.h"
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

/// Held while an encoder opens or closes. SVT-AV1 keeps state of its own
/// across encoders, and opening one while another closes races on it
/// (ThreadSanitizer saw it): one at a time, whichever threads they are on.
std::mutex& lifecycle_mutex() {
    static std::mutex mutex;
    return mutex;
}

struct Candidate {
    const char* name;
    Codec codec;
    bool hardware;
    /// Takes a new AVCodecContext::bit_rate between frames. The others are
    /// opened anew, which costs a keyframe, and takes long enough (SVT-AV1:
    /// 55 ms at 1080p) to be done alongside the encoder at work.
    bool dynamic_bitrate;
    void (*tune)(const VideoEncoderConfig&, AVCodecContext&, av::Dictionary&);
    /// Some of its versions hold pictures back however tuned: checked once a
    /// process, by encoding one (hands_back_at_once()), and passed over if so.
    bool may_hold_back = false;
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
    // Before 2.3 (Ubuntu 24.04 has 1.7) it never waits for a picture in low
    // delay, and FFmpeg asks for one only as it hands in the next: each comes
    // back a picture or two later.
    {"libsvtav1", Codec::AV1, false, false, tune_svtav1, true},
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
    /// With `hw_frames`, an encoder of pictures in that frames context's GPU.
    FfmpegVideoEncoder(const Candidate& candidate, const AVCodec& codec,
                       const VideoEncoderConfig& config, PixelFormat input_format,
                       av::BufferRefPtr hw_frames = nullptr)
        : candidate_(candidate),
          codec_(codec),
          config_(config),
          info_{candidate.name, candidate.codec, candidate.hardware, input_format,
                hw_frames != nullptr},
          hw_frames_(std::move(hw_frames)),
          frame_(av_frame_alloc()),
          packet_(av_packet_alloc()) {}

    ~FfmpegVideoEncoder() override { close_context(std::move(context_)); }

    /// Open the codec for the current config.
    Result<void> open() {
        if (!frame_ || !packet_) return fail(Errc::Internal);
        Result<av::CodecContextPtr> context =
            open_context(candidate_, codec_, config_, info_, hw_frames_.get());
        if (!context) return fail(context.error());
        close_context(std::exchange(context_, std::move(*context)));
        return {};
    }

    const VideoEncoderInfo& info() const noexcept override { return info_; }

    Result<void> encode(const VideoFrame& picture, bool keyframe, Sink sink) override {
        if (picture.format() == PixelFormat::BGRA || picture.width() != config_.width ||
            picture.height() != config_.height || picture.color_space() != config_.color_space) {
            return fail(Errc::InvalidArgument);
        }
        if (!hw_frames_ && (picture.gpu() || picture.format() != info_.input_format)) {
            // In GPU memory, or in the other 4:2:0 layout.
            Result<VideoFrame> converted = convert_frame(picture, info_.input_format);
            if (!converted) return fail(converted.error());
            return encode(*converted, keyframe, sink);
        }
        if (reopen_) {
            // Opened for the config now; one still opening alongside is stale.
            next_ = {};
            open_again_ = false;
            // Whatever the old encoder still holds belongs before this picture.
            if (Result<void> flushed = flush(sink); !flushed) return flushed;
            if (Result<void> opened = open(); !opened) return opened;
            reopen_ = false;
            keyframe = true;
            keyframe_owed_ = false;
        } else if (next_.valid() &&
                   next_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            // The encoder opening alongside starts with a keyframe; one asked
            // for meanwhile comes with it rather than right before it — two in
            // a row, most likely into a path the change is making room on.
            keyframe_owed_ = keyframe_owed_ || keyframe;
            keyframe = false;
        } else if (next_.valid()) {
            if (Result<av::CodecContextPtr> next = next_.get(); next) {
                if (Result<void> flushed = flush(sink); !flushed) return flushed;
                // Closing takes as long as opening: alongside too.
                closing_ = std::async(std::launch::async, &FfmpegVideoEncoder::close_context,
                                      std::exchange(context_, std::move(*next)));
                reopen_ = false;
                keyframe = true;
            }  // else open_context() said why; the encoder at work carries on
            keyframe = std::exchange(keyframe_owed_, false) || keyframe;
            if (std::exchange(open_again_, false)) open_alongside();
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
            apply_bitrate(*context_, config_);
        } else {
            open_alongside();
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
    static void apply_bitrate(AVCodecContext& context, const VideoEncoderConfig& config) {
        const int64_t bitrate = config.bitrate;
        context.bit_rate = bitrate;
        context.rc_max_rate = bitrate;
        context.rc_buffer_size = static_cast<int>(std::min<int64_t>(
            bitrate * kBufferFrames * config.frame_rate_den / config.frame_rate_num,
            std::numeric_limits<int>::max()));
    }

    /// A codec context for `config`, open. Touches nothing of an encoder's,
    /// so any thread may call it.
    static Result<av::CodecContextPtr> open_context(const Candidate& candidate,
                                                    const AVCodec& codec,
                                                    const VideoEncoderConfig& config,
                                                    const VideoEncoderInfo& info,
                                                    AVBufferRef* hw_frames) {
        std::lock_guard lock(lifecycle_mutex());
        av::CodecContextPtr context(avcodec_alloc_context3(&codec));
        if (!context) return fail(Errc::Internal);

        context->width = static_cast<int>(config.width);
        context->height = static_cast<int>(config.height);
        context->pix_fmt = av::pixel_format(info.input_format);
        if (hw_frames) {
            context->pix_fmt = reinterpret_cast<const AVHWFramesContext*>(hw_frames->data)->format;
            context->hw_frames_ctx = av_buffer_ref(hw_frames);
            if (!context->hw_frames_ctx) return fail(Errc::Internal);
        }
        context->time_base = av::kMicroseconds;
        context->framerate = AVRational{static_cast<int>(config.frame_rate_num),
                                        static_cast<int>(config.frame_rate_den)};
        av::set_color_space(*context, config.color_space);
        context->max_b_frames = 0;
        context->gop_size = config.keyframe_interval > 0
                                ? static_cast<int>(config.keyframe_interval)
                                : kNoPeriodicKeyframes;
        apply_bitrate(*context, config);

        av::Dictionary options;
        candidate.tune(config, *context, options);
        const int error = avcodec_open2(context.get(), &codec, options.get());
        if (error < 0) {
            LOG_DEBUG("engine", "encoder " << info.name << " cannot open: " << av::describe(error));
            return fail(error == AVERROR(EINVAL) ? Errc::InvalidArgument : Errc::Unavailable);
        }
        return context;
    }

    /// Close `context`, if any, from any thread.
    static void close_context(av::CodecContextPtr context) {
        if (!context) return;
        std::lock_guard lock(lifecycle_mutex());
        // Say the stream ended: SVT-AV1 complains when closed in mid-stream.
        avcodec_send_frame(context.get(), nullptr);
        context.reset();
    }

    /// Open an encoder for the current config on a thread of its own; encode()
    /// switches to it, with a keyframe, at the first picture after it is ready.
    /// One at a time: a change meanwhile is opened once this one is in.
    void open_alongside() {
        if (next_.valid()) {
            open_again_ = true;
            return;
        }
        next_ =
            std::async(std::launch::async, &FfmpegVideoEncoder::open_context, std::cref(candidate_),
                       std::cref(codec_), config_, info_, hw_frames_.get());
    }

    // Point frame_ at the picture: at a frame of the GPU's that it was copied
    // into there, or else at its planes.
    Result<void> wrap(const VideoFrame& picture, bool keyframe) {
        if (hw_frames_) {
            if (Result<void> filled = av::fill_hw_frame(*hw_frames_, picture, *frame_); !filled) {
                av_frame_unref(frame_.get());
                return filled;
            }
        } else if (Result<void> wrapped = wrap_planes(picture); !wrapped) {
            return wrapped;
        }
        frame_->color_primaries = context_->color_primaries;
        frame_->color_trc = context_->color_trc;
        frame_->colorspace = context_->colorspace;
        frame_->color_range = context_->color_range;

        // Encoders insist on increasing timestamps; two pictures stamped alike
        // are a microsecond apart.
        int64_t pts = picture.capture_time().time_since_epoch().count();
        if (last_pts_ && pts <= *last_pts_) pts = *last_pts_ + 1;
        last_pts_ = pts;
        frame_->pts = pts;
        frame_->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        return {};
    }

    // The frame's buffer holds a copy of the VideoFrame — not of its pixels —
    // so an encoder that keeps the picture keeps its memory alive, and FFmpeg
    // never copies it to be sure.
    Result<void> wrap_planes(const VideoFrame& picture) {
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
        for (size_t i = 0; i < picture.plane_count(); ++i) {
            frame_->data[i] = const_cast<uint8_t*>(picture.plane(i).data);
            frame_->linesize[i] = static_cast<int>(picture.plane(i).stride);
        }
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
    /// The GPU's frames pictures are copied into, for an encoder of them;
    /// before next_, which may still be opening on it.
    av::BufferRefPtr hw_frames_;

    av::CodecContextPtr context_;
    av::FramePtr frame_;
    av::PacketPtr packet_;
    bool reopen_ = false;
    /// The encoder open_alongside() is opening, and whether the config changed
    /// again since it began.
    std::future<Result<av::CodecContextPtr>> next_;
    bool open_again_ = false;
    /// A keyframe was asked for while `next_` opened: it brings one.
    bool keyframe_owed_ = false;
    std::future<void> closing_;  ///< the encoder switched from, being closed
    std::optional<int64_t> last_pts_;
    FrameNumber next_number_ = 0;
};

/// Whether `candidate` hands a picture back as it takes it, as every encoder
/// here must: true unless the table says it may not and one encoded says so.
/// Asked once a process for each.
bool hands_back_at_once(const Candidate& candidate, const AVCodec& codec, PixelFormat format) {
    if (!candidate.may_hold_back) return true;
    static std::mutex mutex;
    static std::map<const Candidate*, bool> checked;
    std::lock_guard lock(mutex);
    if (const auto it = checked.find(&candidate); it != checked.end()) return it->second;

    VideoEncoderConfig config;
    config.codec = candidate.codec;
    config.width = 64;
    config.height = 64;
    config.bitrate = 100'000;
    FfmpegVideoEncoder encoder(candidate, codec, config, format);
    MutableVideoFrame picture = MutableVideoFrame::allocate(format, config.width, config.height);
    for (size_t i = 0; i < plane_count(format); ++i) {
        for (size_t y = 0; y < plane_rows(format, i, config.height); ++y) {
            std::memset(picture.plane(i).data + y * picture.plane(i).stride, 128,
                        plane_row_bytes(format, i, config.width));
        }
    }
    size_t out = 0;
    const bool at_once = encoder.open() &&
                         encoder.encode(std::move(picture).freeze(Timestamp{}), true,
                                        [&](EncodedVideo) { ++out; }) &&
                         out == 1;
    if (!at_once) LOG_INFO("engine", candidate.name << " holds pictures back here: passed over");
    checked.emplace(&candidate, at_once);
    return at_once;
}

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
    const auto opened = [&](std::unique_ptr<FfmpegVideoEncoder> encoder) {
        if (!encoder->open()) return std::unique_ptr<VideoEncoder>();
        LOG_INFO("engine",
                 "video encoder: " << encoder->info().name << " " << config.width << "x"
                                   << config.height << " at " << config.bitrate << " bit/s"
                                   << (encoder->info().gpu_input ? ", GPU pictures" : ""));
        return std::unique_ptr<VideoEncoder>(std::move(encoder));
    };
    // Made for the first candidate that takes the device's frames, and kept
    // for the next if that one will not open on them.
    std::optional<Result<av::BufferRefPtr>> hw_frames;
    for (const Candidate& candidate : kCandidates) {
        if (candidate.codec != config.codec) continue;
        if (!config.encoder.empty() && config.encoder != candidate.name) continue;
        if (candidate.hardware ? !config.allow_hardware : !config.allow_software) continue;

        const AVCodec* codec = avcodec_find_encoder_by_name(candidate.name);
        if (!codec) continue;
        const std::optional<PixelFormat> format = choose_input_format(*codec);
        if (!format || !hands_back_at_once(candidate, *codec, *format)) continue;

        // On the device's frames if it takes them; else, or if it will not
        // open on them (a device of another GPU's), from memory.
        if (config.device && av::takes_hw_frames(*codec, *config.device)) {
            if (!hw_frames)
                hw_frames = av::make_hw_frames(config.device, config.width, config.height);
            if (*hw_frames) {
                av::BufferRefPtr frames(av_buffer_ref((*hw_frames)->get()));
                if (std::unique_ptr<VideoEncoder> encoder =
                        opened(std::make_unique<FfmpegVideoEncoder>(
                            candidate, *codec, config, PixelFormat::NV12, std::move(frames)))) {
                    return encoder;
                }
            }
        }
        if (std::unique_ptr<VideoEncoder> encoder =
                opened(std::make_unique<FfmpegVideoEncoder>(candidate, *codec, config, *format))) {
            return encoder;
        }
    }
    return fail(Errc::Unavailable);
}

}  // namespace backend
}  // namespace ratcast
