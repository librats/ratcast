// PipeWire runs the stream on a thread loop of its own, and calls back on that
// thread with the loop's lock held: everything below that touches PipeWire
// objects does so either there or under that lock.

#include "linux/pipewire_capture.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <optional>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include "capture/frame_pacer.h"
#include "ratcast/engine/video_convert.h"

#include "librats/util/logger.h"

namespace ratcast::pipewire {
namespace {

/// The largest picture we agree to: an 8K screen, with room for odd sizes.
constexpr uint32_t kMaxSide = 16384;

void init_pipewire() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

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

class Capture::Stream {
public:
    Stream(const CaptureConfig& config, Clock& clock, Sink sink, EndHandler ended)
        : config_(config),
          clock_(clock),
          sink_(std::move(sink)),
          ended_(std::move(ended)),
          pacer_(config.max_frame_rate) {}

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    ~Stream() {
        if (!loop_) return;
        // PipeWire (1.6) leaks a stream destroyed while it is still being set
        // up on the daemon, which a capture that ends at once can be.
        if (stream_) {
            pw_thread_loop_lock(loop_);
            for (int second = 0;
                 second < 2 && pw_stream_get_state(stream_, nullptr) == PW_STREAM_STATE_CONNECTING;
                 ++second) {
                pw_thread_loop_timed_wait(loop_, 1);
            }
            pw_thread_loop_unlock(loop_);
        }
        // The loop thread next: past this, nothing calls back, and the rest
        // is torn down here, the way PipeWire's own thread-loop examples do.
        pw_thread_loop_stop(loop_);
        if (stream_) {
            spa_hook_remove(&stream_listener_);
            pw_stream_destroy(stream_);
        }
        if (registry_) {
            spa_hook_remove(&registry_listener_);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
        }
        if (core_) {
            spa_hook_remove(&core_listener_);
            pw_core_disconnect(core_);
        }
        if (context_) pw_context_destroy(context_);
        pw_thread_loop_destroy(loop_);
    }

    Result<void> open() {
        init_pipewire();
        loop_ = pw_thread_loop_new("ratcast-capture", nullptr);
        if (!loop_) return fail(Errc::Internal);
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (!context_ || pw_thread_loop_start(loop_) < 0) return fail(Errc::Internal);

        pw_thread_loop_lock(loop_);
        const Result<void> connected = connect();
        pw_thread_loop_unlock(loop_);
        return connected;
    }

private:
    Result<void> connect() {
        if (config_.fd >= 0) {
            const int fd = fcntl(config_.fd, F_DUPFD_CLOEXEC, 3);
            core_ = fd >= 0 ? pw_context_connect_fd(context_, fd, nullptr, 0) : nullptr;
        } else {
            core_ = pw_context_connect(context_, nullptr, 0);
        }
        if (!core_) {
            LOG_WARN("engine", "cannot connect to PipeWire: " << strerror(errno));
            return fail(Errc::Unavailable);
        }
        static constexpr pw_core_events kCoreEvents = [] {
            pw_core_events events{};
            events.version = PW_VERSION_CORE_EVENTS;
            events.done = [](void* self, uint32_t id, int seq) {
                if (id == PW_ID_CORE) static_cast<Stream*>(self)->on_core_done(seq);
            };
            events.error = [](void* self, uint32_t id, int, int res, const char* message) {
                if (id == PW_ID_CORE) static_cast<Stream*>(self)->on_core_error(res, message);
            };
            return events;
        }();
        pw_core_add_listener(core_, &core_listener_, &kCoreEvents, this);

        // The stream does not tell a node that went from one that is idle:
        // the registry does, and says too whether the node was there at all.
        registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
        if (!registry_) return fail(Errc::Internal);
        static constexpr pw_registry_events kRegistryEvents = [] {
            pw_registry_events events{};
            events.version = PW_VERSION_REGISTRY_EVENTS;
            events.global = [](void* self, uint32_t id, uint32_t, const char* type, uint32_t,
                               const spa_dict*) {
                static_cast<Stream*>(self)->on_global(id, type);
            };
            events.global_remove = [](void* self, uint32_t id) {
                static_cast<Stream*>(self)->on_global_remove(id);
            };
            return events;
        }();
        pw_registry_add_listener(registry_, &registry_listener_, &kRegistryEvents, this);
        registry_sync_ = pw_core_sync(core_, PW_ID_CORE, 0);

        stream_ = pw_stream_new(core_, "ratcast capture",
                                pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                                  "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
        if (!stream_) return fail(Errc::Internal);
        static constexpr pw_stream_events kStreamEvents = [] {
            pw_stream_events events{};
            events.version = PW_VERSION_STREAM_EVENTS;
            events.state_changed = [](void* self, pw_stream_state old, pw_stream_state state,
                                      const char* error) {
                static_cast<Stream*>(self)->on_state_changed(old, state, error);
            };
            events.param_changed = [](void* self, uint32_t id, const spa_pod* param) {
                static_cast<Stream*>(self)->on_param_changed(id, param);
            };
            events.process = [](void* self) { static_cast<Stream*>(self)->on_process(); };
            return events;
        }();
        pw_stream_add_listener(stream_, &stream_listener_, &kStreamEvents, this);

        uint8_t storage[1024];
        spa_pod_builder b{};
        spa_pod_builder_init(&b, storage, sizeof storage);
        const spa_pod* params[] = {enum_format(b, config_.max_frame_rate)};
        // When the node goes, so does the capture: never move on to another.
        const auto flags =
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                         PW_STREAM_FLAG_DONT_RECONNECT);
        if (const int error =
                pw_stream_connect(stream_, PW_DIRECTION_INPUT, config_.node, flags, params, 1);
            error < 0) {
            LOG_WARN("engine", "cannot connect to PipeWire node " << config_.node << ": "
                                                                  << strerror(-error));
            return fail(Errc::Unavailable);
        }
        return {};
    }

    void on_global(uint32_t id, const char* type) {
        if (id == config_.node && type && std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            node_seen_ = true;
        }
    }

    void on_global_remove(uint32_t id) {
        if (id == config_.node) finish({});  // the window closed, or sharing stopped
    }

    /// The registry has listed what there was when the capture started.
    void on_core_done(int seq) {
        if (seq != registry_sync_ || node_seen_) return;
        LOG_WARN("engine", "PipeWire node " << config_.node << " does not exist");
        finish(fail(Errc::Unavailable));
    }

    void on_core_error(int res, const char* message) {
        LOG_WARN("engine",
                 "PipeWire: " << (message ? message : "") << " (" << strerror(-res) << ")");
        finish(fail(Errc::Unavailable));
    }

    void on_state_changed(pw_stream_state old, pw_stream_state state, const char* error) {
        LOG_DEBUG("engine", "capture stream: " << pw_stream_state_as_string(old) << " -> "
                                               << pw_stream_state_as_string(state));
        pw_thread_loop_signal(loop_, false);  // for a teardown waiting on it
        if (state == PW_STREAM_STATE_ERROR) {
            LOG_WARN("engine", "capture stream failed: " << (error ? error : "unknown error"));
            finish(fail(Errc::Unavailable));
        }
    }

    void on_param_changed(uint32_t id, const spa_pod* param) {
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
        pw_stream_update_params(stream_, params, static_cast<uint32_t>(count));
    }

    void on_process() {
        // Only the newest picture matters; older ones go straight back.
        pw_buffer* newest = nullptr;
        while (pw_buffer* buffer = pw_stream_dequeue_buffer(stream_)) {
            if (newest) pw_stream_queue_buffer(stream_, newest);
            newest = buffer;
        }
        if (!newest) return;

        const Timestamp now = clock_.now();
        std::optional<VideoFrame> picture;
        if (pacer_admits(newest, now)) picture = convert(*newest->buffer, now);
        pw_stream_queue_buffer(stream_, newest);
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

    void finish(Result<void> outcome) {
        if (finished_) return;
        finished_ = true;
        if (ended_) ended_(outcome);
    }

    const CaptureConfig config_;
    Clock& clock_;
    const Sink sink_;
    const EndHandler ended_;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook core_listener_{};
    spa_hook registry_listener_{};
    spa_hook stream_listener_{};

    // On the loop thread only.
    int registry_sync_ = 0;
    bool node_seen_ = false;
    std::optional<spa_rectangle> size_;  ///< of the negotiated format
    FramePacer pacer_;
    bool finished_ = false;
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
