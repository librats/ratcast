#include "test_video_node.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

namespace ratcast::test {
namespace {

uint32_t spa_format(PixelFormat format) {
    switch (format) {
        case PixelFormat::I420: return SPA_VIDEO_FORMAT_I420;
        case PixelFormat::NV12: return SPA_VIDEO_FORMAT_NV12;
        case PixelFormat::BGRA: return SPA_VIDEO_FORMAT_BGRx;
        case PixelFormat::YUY2: return SPA_VIDEO_FORMAT_YUY2;
    }
    return SPA_VIDEO_FORMAT_UNKNOWN;
}

/// A screen sends as it changes (a variable rate); a camera at 30 a second.
const spa_pod* format_param(spa_pod_builder& b, uint32_t id, uint32_t width, uint32_t height,
                            PixelFormat format, bool camera) {
    spa_pod_frame frame;
    spa_pod_builder_push_object(&b, &frame, SPA_TYPE_OBJECT_Format, id);
    const spa_rectangle size{width, height};
    const spa_fraction rate = camera ? spa_fraction{30, 1} : spa_fraction{0, 1};
    spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format(format)),
                        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), SPA_FORMAT_VIDEO_framerate,
                        SPA_POD_Fraction(&rate), 0);
    return static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &frame));
}

/// Rows padded to this, as devices pad them.
constexpr size_t kRowAlignment = 64;

}  // namespace

class TestVideoNode::Impl {
public:
    Impl(uint32_t width, uint32_t height, Options options)
        : width_(width), height_(height), options_(std::move(options)) {}

    ~Impl() {
        if (!loop_) return;
        pw_thread_loop_stop(loop_);
        if (stream_) {
            spa_hook_remove(&listener_);
            pw_stream_destroy(stream_);
        }
        if (core_) pw_core_disconnect(core_);
        if (context_) pw_context_destroy(context_);
        pw_thread_loop_destroy(loop_);
    }

    bool open() {
        pw_init(nullptr, nullptr);
        loop_ = pw_thread_loop_new("test-video-node", nullptr);
        if (!loop_) return false;
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (!context_ || pw_thread_loop_start(loop_) < 0) return false;

        pw_thread_loop_lock(loop_);
        const bool ready = connect() && wait_for_node();
        pw_thread_loop_unlock(loop_);
        return ready;
    }

    uint32_t node_id() const { return node_id_; }

    void show(const VideoFrame& picture, std::optional<Crop> crop) {
        pw_thread_loop_lock(loop_);
        pending_ = picture;
        crop_ = crop;
        pw_stream_trigger_process(stream_);
        pw_thread_loop_unlock(loop_);
    }

    void resize(uint32_t width, uint32_t height) {
        pw_thread_loop_lock(loop_);
        width_ = width;
        height_ = height;
        uint8_t storage[512];
        spa_pod_builder b{};
        spa_pod_builder_init(&b, storage, sizeof storage);
        const spa_pod* params[] = {
            format_param(b, SPA_PARAM_EnumFormat, width, height, options_.format, options_.camera)};
        pw_stream_update_params(stream_, params, 1);
        pw_thread_loop_unlock(loop_);
    }

private:
    bool connect() {
        core_ = pw_context_connect(context_, nullptr, 0);
        if (!core_) return false;
        stream_ = pw_stream_new(
            core_, options_.name.c_str(),
            pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_TYPE, "Video",
                              PW_KEY_MEDIA_CATEGORY, "Source", PW_KEY_MEDIA_ROLE,
                              options_.camera ? "Camera" : "Screen", PW_KEY_NODE_NAME,
                              options_.name.c_str(), PW_KEY_NODE_DESCRIPTION, options_.name.c_str(),
                              nullptr));
        if (!stream_) return false;

        static constexpr pw_stream_events kEvents = [] {
            pw_stream_events events{};
            events.version = PW_VERSION_STREAM_EVENTS;
            events.state_changed = [](void* self, pw_stream_state, pw_stream_state, const char*) {
                pw_thread_loop_signal(static_cast<Impl*>(self)->loop_, false);
            };
            events.param_changed = [](void* self, uint32_t id, const spa_pod* param) {
                static_cast<Impl*>(self)->on_param_changed(id, param);
            };
            events.process = [](void* self) { static_cast<Impl*>(self)->on_process(); };
            return events;
        }();
        pw_stream_add_listener(stream_, &listener_, &kEvents, this);

        uint8_t storage[512];
        spa_pod_builder b{};
        spa_pod_builder_init(&b, storage, sizeof storage);
        const spa_pod* params[] = {format_param(b, SPA_PARAM_EnumFormat, width_, height_,
                                                options_.format, options_.camera)};
        const auto flags =
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS);
        return pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) >= 0;
    }

    /// Until the daemon has given the node an id. Called with the lock held.
    bool wait_for_node() {
        for (int attempt = 0; attempt < 50; ++attempt) {
            const pw_stream_state state = pw_stream_get_state(stream_, nullptr);
            if (state == PW_STREAM_STATE_ERROR) return false;
            node_id_ = pw_stream_get_node_id(stream_);
            if (state >= PW_STREAM_STATE_PAUSED && node_id_ != SPA_ID_INVALID) return true;
            pw_thread_loop_timed_wait(loop_, 1);
        }
        return false;
    }

    void on_param_changed(uint32_t id, const spa_pod* param) {
        if (id != SPA_PARAM_Format || !param) return;
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) return;
        size_ = info.size;
        // I420's chroma rows are half the luma's, one block or not.
        const size_t planes = plane_count(options_.format);
        const size_t luma = (plane_row_bytes(options_.format, 0, size_.width) + kRowAlignment) /
                            kRowAlignment * kRowAlignment;
        for (size_t i = 0; i < planes; ++i) {
            strides_[i] = i > 0 && options_.format == PixelFormat::I420 ? luma / 2 : luma;
            sizes_[i] = strides_[i] * plane_rows(options_.format, i, size_.height);
        }
        const size_t blocks = options_.block_per_plane ? planes : 1;
        size_t block_size = 0;
        for (size_t i = 0; i < planes; ++i) block_size = std::max(block_size, sizes_[i]);
        if (blocks == 1) {
            block_size = 0;
            for (size_t i = 0; i < planes; ++i) block_size += sizes_[i];
        }

        uint8_t storage[1024];
        spa_pod_builder b{};
        spa_pod_builder_init(&b, storage, sizeof storage);
        const spa_pod* params[3];
        spa_pod_frame frame;
        spa_pod_builder_push_object(&b, &frame, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
        spa_pod_builder_add(
            &b, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(static_cast<int32_t>(blocks)),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(static_cast<int32_t>(block_size)),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(static_cast<int32_t>(strides_[0])),
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)), 0);
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
        pw_stream_update_params(stream_, params, 3);
    }

    void on_process() {
        if (pending_.empty()) return;
        pw_buffer* buffer = pw_stream_dequeue_buffer(stream_);
        if (!buffer) return;
        spa_buffer& b = *buffer->buffer;
        const VideoFrame picture = std::move(pending_);
        pending_ = VideoFrame{};
        const size_t planes = plane_count(options_.format);
        const bool apart = b.n_datas >= planes && planes > 1;
        if (!b.datas[0].data || (planes > 1 && options_.block_per_plane && !apart) ||
            picture.format() != options_.format || picture.width() != size_.width ||
            picture.height() != size_.height) {
            pw_stream_queue_buffer(stream_, buffer);  // a picture of the old size: dropped
            return;
        }

        size_t offset = 0;
        for (size_t i = 0; i < planes; ++i) {
            spa_data& data = b.datas[apart ? i : 0];
            uint8_t* plane = static_cast<uint8_t*>(data.data) + (apart ? 0 : offset);
            for (size_t y = 0; y < plane_rows(options_.format, i, size_.height); ++y) {
                std::memcpy(plane + y * strides_[i],
                            picture.plane(i).data + y * picture.plane(i).stride,
                            plane_row_bytes(options_.format, i, size_.width));
            }
            if (apart || i == 0) {
                data.chunk->offset = 0;
                data.chunk->stride = static_cast<int32_t>(strides_[i]);
                data.chunk->flags = SPA_CHUNK_FLAG_NONE;
            }
            data.chunk->size = static_cast<uint32_t>(apart ? sizes_[i] : offset + sizes_[i]);
            offset += sizes_[i];
        }

        if (auto* header = static_cast<spa_meta_header*>(
                spa_buffer_find_meta_data(&b, SPA_META_Header, sizeof(spa_meta_header)))) {
            *header = {};
            header->pts = picture.capture_time().time_since_epoch().count() * 1000;
            header->seq = sequence_++;
        }
        if (auto* region = static_cast<spa_meta_region*>(
                spa_buffer_find_meta_data(&b, SPA_META_VideoCrop, sizeof(spa_meta_region)))) {
            region->region = crop_ ? spa_region{{crop_->x, crop_->y}, {crop_->width, crop_->height}}
                                   : spa_region{};
        }
        pw_stream_queue_buffer(stream_, buffer);
    }

    uint32_t width_, height_;
    const Options options_;
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};
    uint32_t node_id_ = SPA_ID_INVALID;

    // Under the loop lock.
    spa_rectangle size_{};
    std::array<size_t, 3> strides_{};  ///< of each plane
    std::array<size_t, 3> sizes_{};
    VideoFrame pending_;
    std::optional<Crop> crop_;
    uint64_t sequence_ = 0;
};

std::unique_ptr<TestVideoNode> TestVideoNode::create(uint32_t width, uint32_t height,
                                                     Options options) {
    auto impl = std::make_unique<Impl>(width, height, std::move(options));
    if (!impl->open()) return nullptr;
    return std::unique_ptr<TestVideoNode>(new TestVideoNode(std::move(impl)));
}

TestVideoNode::TestVideoNode(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TestVideoNode::~TestVideoNode() = default;

uint32_t TestVideoNode::node_id() const noexcept { return impl_->node_id(); }

void TestVideoNode::show(const VideoFrame& picture, std::optional<Crop> crop) {
    impl_->show(picture, crop);
}

void TestVideoNode::resize(uint32_t width, uint32_t height) { impl_->resize(width, height); }

}  // namespace ratcast::test
