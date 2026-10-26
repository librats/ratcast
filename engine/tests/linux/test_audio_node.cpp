#include "test_audio_node.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

namespace ratcast::test {
namespace {

constexpr uint32_t kRate = 48'000;
constexpr double kPitch = 440.0;

}  // namespace

class TestAudioNode::Impl {
public:
    Impl(Kind kind, std::string name) : kind_(kind), name_(std::move(name)) {}

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
        loop_ = pw_thread_loop_new("test-audio-node", nullptr);
        if (!loop_) return false;
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (!context_ || pw_thread_loop_start(loop_) < 0) return false;

        pw_thread_loop_lock(loop_);
        const bool ready = connect() && wait_for_node();
        pw_thread_loop_unlock(loop_);
        return ready;
    }

    uint32_t node_id() const { return node_id_; }
    float loudest() const { return loudest_.load(std::memory_order_relaxed); }

private:
    bool connect() {
        core_ = pw_context_connect(context_, nullptr, 0);
        if (!core_) return false;
        const bool source = kind_ == Kind::Source;
        stream_ = pw_stream_new(
            core_, name_.c_str(),
            pw_properties_new(PW_KEY_MEDIA_CLASS, source ? "Audio/Source" : "Audio/Sink",
                              PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_NODE_NAME, name_.c_str(),
                              PW_KEY_NODE_DESCRIPTION, name_.c_str(), nullptr));
        if (!stream_) return false;

        static constexpr pw_stream_events kEvents = [] {
            pw_stream_events events{};
            events.version = PW_VERSION_STREAM_EVENTS;
            events.state_changed = [](void* self, pw_stream_state, pw_stream_state, const char*) {
                pw_thread_loop_signal(static_cast<Impl*>(self)->loop_, false);
            };
            events.process = [](void* self) { static_cast<Impl*>(self)->on_process(); };
            return events;
        }();
        pw_stream_add_listener(stream_, &listener_, &kEvents, this);

        uint8_t storage[512];
        spa_pod_builder b{};
        spa_pod_builder_init(&b, storage, sizeof storage);
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = kRate;
        info.channels = 1;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        const spa_pod* params[] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
        const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS);
        return pw_stream_connect(stream_, source ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
                                 PW_ID_ANY, flags, params, 1) >= 0;
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

    void on_process() {
        pw_buffer* buffer = pw_stream_dequeue_buffer(stream_);
        if (!buffer) return;
        spa_data& data = buffer->buffer->datas[0];
        if (data.data && data.chunk) {
            auto* samples = static_cast<float*>(data.data);
            if (kind_ == Kind::Source) {
                size_t length = data.maxsize / sizeof(float);
                if (buffer->requested > 0) length = std::min<size_t>(length, buffer->requested);
                for (size_t i = 0; i < length; ++i, ++sent_) {
                    samples[i] =
                        kLevel *
                        float(std::sin(2 * std::numbers::pi * kPitch * double(sent_) / kRate));
                }
                data.chunk->offset = 0;
                data.chunk->stride = sizeof(float);
                data.chunk->size = uint32_t(length * sizeof(float));
            } else {
                const size_t offset = std::min<size_t>(data.chunk->offset, data.maxsize);
                const size_t length =
                    std::min<size_t>(data.chunk->size, data.maxsize - offset) / sizeof(float);
                float loudest = loudest_.load(std::memory_order_relaxed);
                for (size_t i = 0; i < length; ++i)
                    loudest = std::max(loudest, std::abs(samples[offset / sizeof(float) + i]));
                loudest_.store(loudest, std::memory_order_relaxed);
            }
        }
        pw_stream_queue_buffer(stream_, buffer);
    }

    const Kind kind_;
    const std::string name_;
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};
    uint32_t node_id_ = SPA_ID_INVALID;
    uint64_t sent_ = 0;  ///< samples of the tone, on the data thread
    std::atomic<float> loudest_{0.0f};
};

std::unique_ptr<TestAudioNode> TestAudioNode::create(Kind kind, const std::string& name) {
    auto impl = std::make_unique<Impl>(kind, name);
    if (!impl->open()) return nullptr;
    return std::unique_ptr<TestAudioNode>(new TestAudioNode(std::move(impl)));
}

TestAudioNode::TestAudioNode(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TestAudioNode::~TestAudioNode() = default;

uint32_t TestAudioNode::node_id() const noexcept { return impl_->node_id(); }
float TestAudioNode::loudest() const noexcept { return impl_->loudest(); }

}  // namespace ratcast::test
