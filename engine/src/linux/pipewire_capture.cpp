// The stream runs on a StreamThread, which calls back on its loop's thread with
// the loop's lock held: everything below that touches PipeWire objects does so
// there.

#include "linux/pipewire_capture.h"

#include <algorithm>
#include <optional>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include "capture/frame_pacer.h"
#include "linux/pipewire_stream.h"
#include "ratcast/engine/video_convert.h"

#include "librats/util/logger.h"

namespace ratcast::pipewire {
namespace {

/// The largest picture we agree to: an 8K screen, with room for odd sizes.
constexpr uint32_t kMaxSide = 16384;

/// What the capture asks of the node: BGRx (or BGRA) in shared memory, any
/// size, at most `max_rate` pictures a second (0 for any). Compositors send
/// pictures as the screen changes, so the frame rate itself is variable: 0/1.
const spa_pod* enum_format(spa_pod_builder& b, uint32_t max_rate) {
    spa_pod_frame frame;
    spa_pod_builder_push_object(&b, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format,
                        SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                                               SPA_VIDEO_FORMAT_BGRA),
                        0);

    const spa_rectangle size_default{1920, 1080};
    const spa_rectangle size_min{1, 1};
    const spa_rectangle size_max{kMaxSide, kMaxSide};
    spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_size,
                        SPA_POD_CHOICE_RANGE_Rectangle(&size_default, &size_min, &size_max), 0);

    const spa_fraction variable{0, 1};
    spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable), 0);
    if (max_rate > 0) {
        const spa_fraction rate_max{max_rate, 1};
        const spa_fraction rate_min{1, 1};
        spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_maxFramerate,
                            SPA_POD_CHOICE_RANGE_Fraction(&rate_max, &rate_min, &rate_max), 0);
    }
    return static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &frame));
}

/// Buffers in memory we can read (no DMA-BUF: that needs a GPU to read), with
/// the metadata that says whether a picture is whole and which part to show.
int buffer_params(spa_pod_builder& b, const spa_pod* params[3]) {
    spa_pod_frame frame;
    spa_pod_builder_push_object(&b, &frame, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
    spa_pod_builder_add(&b, SPA_PARAM_BUFFERS_dataType,
                        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)),
                        0);
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &frame));

    const auto meta = [&b](uint32_t type, size_t size) {
        spa_pod_frame f;
        spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
        spa_pod_builder_add(&b, SPA_PARAM_META_type, SPA_POD_Id(type), SPA_PARAM_META_size,
                            SPA_POD_Int(static_cast<int32_t>(size)), 0);
        return static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &f));
    };
    params[1] = meta(SPA_META_Header, sizeof(spa_meta_header));
    params[2] = meta(SPA_META_VideoCrop, sizeof(spa_meta_region));
    return 3;
}

}  // namespace

class Capture::Stream final : private StreamThread::Events {
public:
    Stream(const CaptureConfig& config, Clock& clock, Sink sink, EndHandler ended)
        : config_(config),
          clock_(clock),
          sink_(std::move(sink)),
          pacer_(config.max_frame_rate),
          thread_(*this, std::move(ended)) {}

    Result<void> open() {
        uint8_t storage[1024];
        spa_pod_builder b{};
        spa_pod_builder_init(&b, storage, sizeof storage);
        const spa_pod* params[] = {enum_format(b, config_.max_frame_rate)};
        StreamThread::Config stream;
        stream.fd = config_.fd;
        stream.node = config_.node;
        stream.name = "ratcast-capture";
        return thread_.open(stream,
                            pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                              "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr),
                            params);
    }

private:
    void on_param_changed(uint32_t id, const spa_pod* param) override {
        if (id != SPA_PARAM_Format) return;
        size_.reset();
        if (!param) return;

        uint32_t media_type = 0;
        uint32_t media_subtype = 0;
        spa_video_info_raw info{};
        if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
            media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw ||
            spa_format_video_raw_parse(param, &info) < 0 || info.size.width == 0 ||
            info.size.height == 0) {
            return;
        }
        size_ = info.size;
        LOG_INFO("engine", "capturing " << info.size.width << "x" << info.size.height);

        uint8_t storage[1024];
        spa_pod_builder b{};
        spa_pod_builder_init(&b, storage, sizeof storage);
        const spa_pod* params[3];
        const int count = buffer_params(b, params);
        pw_stream_update_params(thread_.stream(), params, static_cast<uint32_t>(count));
    }

    void on_process() override {
        pw_stream* stream = thread_.stream();
        // Only the newest picture matters; older ones go straight back.
        pw_buffer* newest = nullptr;
        while (pw_buffer* buffer = pw_stream_dequeue_buffer(stream)) {
            if (newest) pw_stream_queue_buffer(stream, newest);
            newest = buffer;
        }
        if (!newest) return;

        const Timestamp now = clock_.now();
        std::optional<VideoFrame> picture;
        if (pacer_admits(newest, now)) picture = convert(*newest->buffer, now);
        pw_stream_queue_buffer(stream, newest);
        if (picture) sink_(std::move(*picture));
    }

    /// Whether `buffer` holds a whole picture that the pacer lets through.
    bool pacer_admits(const pw_buffer* buffer, Timestamp now) {
        const spa_buffer& b = *buffer->buffer;
        if (!size_ || b.n_datas < 1 || !b.datas[0].chunk || b.datas[0].chunk->size == 0 ||
            (b.datas[0].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) != 0) {
            return false;
        }
        const auto* header = static_cast<const spa_meta_header*>(
            spa_buffer_find_meta_data(&b, SPA_META_Header, sizeof(spa_meta_header)));
        if (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED) != 0) return false;
        return pacer_.admit(now);
    }

    /// The picture in `buffer`, cropped as its metadata says and converted.
    std::optional<VideoFrame> convert(const spa_buffer& buffer, Timestamp now) {
        const spa_data& data = buffer.datas[0];
        if (!data.data) {
            // DMA-BUF, which we did not ask for: nothing we can read here.
            LOG_WARN("engine", "capture buffer of type " << data.type << " is not mapped");
            return std::nullopt;
        }

        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t width = size_->width;
        uint32_t height = size_->height;
        const auto* crop = static_cast<const spa_meta_region*>(
            spa_buffer_find_meta_data(&buffer, SPA_META_VideoCrop, sizeof(spa_meta_region)));
        if (crop && spa_meta_region_is_valid(crop) && crop->region.position.x >= 0 &&
            crop->region.position.y >= 0) {
            x = std::min(static_cast<uint32_t>(crop->region.position.x), width - 1);
            y = std::min(static_cast<uint32_t>(crop->region.position.y), height - 1);
            width = std::min(crop->region.size.width, width - x);
            height = std::min(crop->region.size.height, height - y);
        }
        if (config_.format != PixelFormat::BGRA) {  // 4:2:0 wants even sizes
            width &= ~1u;
            height &= ~1u;
        }
        if (width == 0 || height == 0) return std::nullopt;

        const int32_t chunk_stride = data.chunk->stride;
        const size_t stride =
            chunk_stride > 0 ? static_cast<size_t>(chunk_stride) : size_t{size_->width} * 4;
        const size_t offset = std::min(data.chunk->offset, data.maxsize);
        const size_t first = offset + y * stride + size_t{x} * 4;
        const size_t end = first + (height - 1) * stride + size_t{width} * 4;
        if (end > data.maxsize) {
            LOG_WARN("engine", "capture buffer is smaller than its picture: dropped");
            return std::nullopt;
        }

        const VideoFrame::Plane plane{static_cast<const uint8_t*>(data.data) + first, stride};
        const VideoFrame view(PixelFormat::BGRA, width, height, {&plane, 1}, nullptr, now);
        Result<VideoFrame> picture = convert_frame(view, config_.format, config_.color_space);
        if (!picture) return std::nullopt;
        return std::move(*picture);
    }

    const CaptureConfig config_;
    Clock& clock_;
    const Sink sink_;

    // On the loop thread only.
    std::optional<spa_rectangle> size_;  ///< of the negotiated format
    FramePacer pacer_;

    StreamThread thread_;  ///< last: it stops calling back before the rest goes
};

Capture::Capture(CaptureConfig config, Clock& clock) : config_(config), clock_(clock) {}

Capture::~Capture() { stop(); }

Result<void> Capture::start(Sink sink, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (stream_) return fail(Errc::InvalidArgument);
    auto stream = std::make_unique<Stream>(config_, clock_, std::move(sink), std::move(ended));
    if (Result<void> opened = stream->open(); !opened) return opened;
    stream_ = std::move(stream);
    return {};
}

void Capture::stop() {
    std::lock_guard lock(mutex_);
    stream_.reset();
}

}  // namespace ratcast::pipewire
