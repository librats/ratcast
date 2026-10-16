// A video file through libavformat and libavcodec, paced by its timestamps
// on a thread of its own. Pictures the decoder gives in the format asked for
// go out as they are, sharing FFmpeg's buffers; anything else (another 4:2:0
// layout, 10 bits, 4:4:4) goes through libswscale once.

#include "ratcast/engine/file_source.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include "ffmpeg/av.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include "librats/util/logger.h"

namespace ratcast {
namespace {

using std::chrono::steady_clock;

/// How far behind its timestamps playback may fall before it gives up on
/// them and carries on from now, rather than rushing pictures out to catch up.
constexpr auto kMaxLag = std::chrono::milliseconds(100);

struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const noexcept { avformat_close_input(&context); }
};
struct SwsContextDeleter {
    void operator()(SwsContext* context) const noexcept { sws_freeContext(context); }
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

/// A decoded picture and when it is due, in microseconds of file time.
struct Decoded {
    VideoFrame picture;
    int64_t pts;
    int64_t duration;
};

class FfmpegFileSource final : public FileSource {
public:
    FfmpegFileSource(const FileSourceOptions& options, Clock& clock, FormatContextPtr format,
                     av::CodecContextPtr decoder, int stream)
        : options_(options),
          clock_(clock),
          format_(std::move(format)),
          decoder_(std::move(decoder)),
          stream_(stream),
          time_base_(format_->streams[stream]->time_base),
          frame_(av_frame_alloc()),
          packet_(av_packet_alloc()) {
        const AVStream& s = *format_->streams[stream];
        info_.width = static_cast<uint32_t>(decoder_->width);
        info_.height = static_cast<uint32_t>(decoder_->height);
        if (options_.format != PixelFormat::BGRA) {  // 4:2:0 wants even sizes
            info_.width &= ~1u;
            info_.height &= ~1u;
        }
        const AVRational rate = s.avg_frame_rate.num > 0 ? s.avg_frame_rate : s.r_frame_rate;
        if (rate.num > 0 && rate.den > 0) {
            info_.frame_rate_num = static_cast<uint32_t>(rate.num);
            info_.frame_rate_den = static_cast<uint32_t>(rate.den);
        }
        for (unsigned i = 0; i < format_->nb_streams; ++i) {
            if (static_cast<int>(i) != stream) format_->streams[i]->discard = AVDISCARD_ALL;
        }
    }

    ~FfmpegFileSource() override { stop(); }

    bool valid() const noexcept { return frame_ && packet_ && info_.width > 0 && info_.height > 0; }

    const Info& info() const noexcept override { return info_; }

    Result<void> start(Sink sink, EndHandler ended) override {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) return fail(Errc::InvalidArgument);
        thread_ = std::jthread([this, sink = std::move(sink), ended = std::move(ended)](
                                   std::stop_token stop) { play(stop, sink, ended); });
        return {};
    }

    void stop() override {
        std::lock_guard lock(mutex_);
        if (!thread_.joinable()) return;
        thread_.request_stop();
        thread_.join();
        thread_ = {};
    }

private:
    void play(const std::stop_token& stop, const Sink& sink, const EndHandler& ended) {
        Result<void> outcome = rewind();
        // Where file time `origin_pts` falls on the steady clock; every
        // picture is due that far after it.
        std::optional<steady_clock::time_point> origin;
        int64_t origin_pts = 0;
        // Passes after the first play on from where the one before ended.
        int64_t offset = 0;
        int64_t end_of_pass = 0;
        std::optional<int64_t> pass_start;  ///< file time of the pass's first picture

        while (outcome && !stop.stop_requested()) {
            Result<std::optional<Decoded>> next = decode();
            if (!next) {
                outcome = fail(next.error());
                break;
            }
            if (!*next) {                                          // the end of the file
                if (!pass_start) outcome = fail(Errc::Malformed);  // not one picture in it
                if (!options_.loop || !outcome) break;
                offset = end_of_pass - *pass_start;
                pass_start.reset();
                outcome = rewind();
                continue;
            }

            Decoded& decoded = **next;
            if (!pass_start) pass_start = decoded.pts;
            const int64_t pts = decoded.pts + offset;
            end_of_pass = std::max(end_of_pass, pts + decoded.duration);

            const steady_clock::time_point now = steady_clock::now();
            if (!origin) {
                origin = now;
                origin_pts = pts;
            }
            steady_clock::time_point due = *origin + std::chrono::microseconds(pts - origin_pts);
            if (now - due > kMaxLag) {  // decoding cannot keep up: carry on from now
                *origin += now - due;
                due = now;
            }
            if (!wait_until(stop, due)) break;
            decoded.picture.set_capture_time(clock_.now());
            sink(std::move(decoded.picture));
        }

        if (!stop.stop_requested() && ended) {
            if (!outcome) LOG_WARN("engine", "file source stopped: " << to_string(outcome.error()));
            ended(outcome);
        }
    }

    /// Sleep until `due`; false if stopped first.
    bool wait_until(const std::stop_token& stop, steady_clock::time_point due) {
        std::unique_lock lock(sleep_mutex_);
        sleep_.wait_until(lock, stop, due, [] { return false; });
        return !stop.stop_requested();
    }

    /// Back to the start of the file, with nothing left in the decoder.
    Result<void> rewind() {
        const int64_t start = format_->streams[stream_]->start_time;
        const int error = av_seek_frame(format_.get(), stream_, start != AV_NOPTS_VALUE ? start : 0,
                                        AVSEEK_FLAG_BACKWARD);
        if (error < 0 && seeked_) {  // the first pass needs no seek
            LOG_WARN("engine", "file source cannot seek: " << av::describe(error));
            return fail(Errc::Unsupported);
        }
        seeked_ = true;
        avcodec_flush_buffers(decoder_.get());
        draining_ = false;
        last_pts_.reset();
        return {};
    }

    /// The next picture, or nullopt at the end of the file.
    Result<std::optional<Decoded>> decode() {
        for (;;) {
            const int received = avcodec_receive_frame(decoder_.get(), frame_.get());
            if (received == 0) return picture();
            if (received == AVERROR_EOF) return std::optional<Decoded>{};
            if (received != AVERROR(EAGAIN)) return fail(av::errc_of(received));

            const int read = av_read_frame(format_.get(), packet_.get());
            if (read == AVERROR_EOF) {
                if (draining_) return std::optional<Decoded>{};
                draining_ = true;
                avcodec_send_packet(decoder_.get(), nullptr);  // let the last pictures out
                continue;
            }
            if (read < 0) {
                LOG_WARN("engine", "file source cannot read: " << av::describe(read));
                return fail(Errc::Malformed);
            }
            if (packet_->stream_index == stream_) {
                const int sent = avcodec_send_packet(decoder_.get(), packet_.get());
                if (sent < 0 && sent != AVERROR(EAGAIN)) {
                    // A damaged packet: skip it, as a player would.
                    LOG_DEBUG("engine", "file source skips a packet: " << av::describe(sent));
                }
            }
            av_packet_unref(packet_.get());
        }
    }

    /// frame_ as a picture in the format asked for, with its time.
    Result<std::optional<Decoded>> picture() {
        const int64_t frame_duration =
            frame_->duration > 0 ? av_rescale_q(frame_->duration, time_base_, av::kMicroseconds)
            : info_.frame_rate_num > 0
                ? int64_t{1'000'000} * info_.frame_rate_den / info_.frame_rate_num
                : 40'000;
        int64_t pts = frame_->best_effort_timestamp;
        pts = pts != AV_NOPTS_VALUE ? av_rescale_q(pts, time_base_, av::kMicroseconds)
                                    : last_pts_.value_or(-frame_duration) + frame_duration;
        last_pts_ = pts;

        const uint32_t width = std::min(info_.width, static_cast<uint32_t>(frame_->width));
        const uint32_t height = std::min(info_.height, static_cast<uint32_t>(frame_->height));
        std::optional<VideoFrame> picture;
        if (av::pixel_format(static_cast<AVPixelFormat>(frame_->format)) == options_.format) {
            picture = av::take_picture(*frame_, width, height, Timestamp{});
        } else {
            picture = convert(width, height);
        }
        av_frame_unref(frame_.get());
        if (!picture) return fail(Errc::Unsupported);
        return std::optional<Decoded>(Decoded{std::move(*picture), pts, frame_duration});
    }

    /// frame_ converted by libswscale, keeping its colour space.
    std::optional<VideoFrame> convert(uint32_t width, uint32_t height) {
        const auto source = static_cast<AVPixelFormat>(frame_->format);
        scaler_.reset(sws_getCachedContext(scaler_.release(), frame_->width, frame_->height, source,
                                           static_cast<int>(width), static_cast<int>(height),
                                           av::pixel_format(options_.format), SWS_BILINEAR, nullptr,
                                           nullptr, nullptr));
        if (!scaler_) {
            LOG_WARN("engine", "file source cannot convert pixel format " << frame_->format);
            return std::nullopt;
        }
        const ColorSpace colors = av::color_space(*frame_);
        const bool full = colors.range == ColorRange::Full;
        const int* table = sws_getCoefficients(colors.matrix == ColorMatrix::BT601 ? SWS_CS_ITU601
                                                                                   : SWS_CS_ITU709);
        const bool to_rgb = options_.format == PixelFormat::BGRA;
        sws_setColorspaceDetails(scaler_.get(), table, full, table, to_rgb || full, 0, 1 << 16,
                                 1 << 16);

        MutableVideoFrame out = MutableVideoFrame::allocate(options_.format, width, height);
        uint8_t* planes[VideoFrame::kMaxPlanes] = {};
        int strides[VideoFrame::kMaxPlanes] = {};
        for (size_t i = 0; i < plane_count(options_.format); ++i) {
            planes[i] = out.plane(i).data;
            strides[i] = static_cast<int>(out.plane(i).stride);
        }
        sws_scale(scaler_.get(), frame_->data, frame_->linesize, 0, frame_->height, planes,
                  strides);
        return std::move(out).freeze(Timestamp{}, to_rgb ? ColorSpace{} : colors);
    }

    const FileSourceOptions options_;
    Clock& clock_;
    Info info_;

    std::mutex mutex_;  ///< serialises start() and stop()
    std::jthread thread_;
    std::mutex sleep_mutex_;
    std::condition_variable_any sleep_;

    // On the playing thread only.
    FormatContextPtr format_;
    av::CodecContextPtr decoder_;
    const int stream_;
    const AVRational time_base_;
    av::FramePtr frame_;
    av::PacketPtr packet_;
    std::unique_ptr<SwsContext, SwsContextDeleter> scaler_;
    bool seeked_ = false;
    bool draining_ = false;
    std::optional<int64_t> last_pts_;
};

}  // namespace

Result<std::unique_ptr<FileSource>> open_file_source(const FileSourceOptions& options,
                                                     Clock& clock) {
    AVFormatContext* raw = nullptr;
    if (const int error = avformat_open_input(&raw, options.path.c_str(), nullptr, nullptr);
        error < 0) {
        LOG_WARN("engine", "cannot open " << options.path << ": " << av::describe(error));
        return fail(error == AVERROR_INVALIDDATA ? Errc::Unsupported : Errc::Unavailable);
    }
    FormatContextPtr format(raw);
    if (avformat_find_stream_info(format.get(), nullptr) < 0) return fail(Errc::Unsupported);

    const int stream = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) {
        LOG_WARN("engine", options.path << " has no video");
        return fail(Errc::Unsupported);
    }
    const AVCodecParameters& parameters = *format->streams[stream]->codecpar;
    const AVCodec* codec = av::find_decoder(parameters.codec_id);
    if (!codec) {
        LOG_WARN("engine", "no decoder for " << avcodec_get_name(parameters.codec_id));
        return fail(Errc::Unavailable);
    }
    av::CodecContextPtr decoder(avcodec_alloc_context3(codec));
    if (!decoder || avcodec_parameters_to_context(decoder.get(), &parameters) < 0) {
        return fail(Errc::Internal);
    }
    decoder->pkt_timebase = format->streams[stream]->time_base;
    decoder->thread_count = 0;  // as many as suit: a file is decoded ahead, latency is no issue
    if (const int error = avcodec_open2(decoder.get(), codec, nullptr); error < 0) {
        LOG_WARN("engine", "cannot open " << codec->name << ": " << av::describe(error));
        return fail(Errc::Unavailable);
    }

    auto source = std::make_unique<FfmpegFileSource>(options, clock, std::move(format),
                                                     std::move(decoder), stream);
    if (!source->valid()) return fail(Errc::Unsupported);
    const FileSource::Info& info = source->info();
    LOG_INFO("engine", "file source: " << options.path << ", " << codec->name << " " << info.width
                                       << "x" << info.height << " at " << info.frame_rate_num << "/"
                                       << info.frame_rate_den << " fps");
    return std::unique_ptr<FileSource>(std::move(source));
}

}  // namespace ratcast
