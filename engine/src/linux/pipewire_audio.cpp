// Both streams process on PipeWire's realtime data thread (RT_PROCESS): a
// buffer is filled or read there and handed straight back, without the loop's
// lock, so the sink or the pull is the only thing that can make the device
// wait.

#include "linux/pipewire_audio.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include "audio/sample_clock.h"
#include "linux/pipewire_stream.h"

namespace ratcast::pipewire {
namespace {

/// 32-bit float samples, interleaved, at the rate and channels of `format`.
const spa_pod* audio_format(spa_pod_builder& b, const AudioFormat& format) {
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = format.sample_rate;
    info.channels = format.channels;
    if (format.channels == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    return spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
}

/// What every audio stream here says of itself: its kind, and the buffer
/// size it asks the graph for ("480/48000").
pw_properties* audio_properties(const char* category, const AudioFormat& format, Duration latency) {
    char node_latency[32];
    std::snprintf(node_latency, sizeof node_latency, "%zu/%u",
                  audio_length(latency, format.sample_rate), format.sample_rate);
    return pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, category,
                             PW_KEY_NODE_LATENCY, node_latency, nullptr);
}

size_t bytes_per_sample_time(const AudioFormat& format) { return sizeof(float) * format.channels; }

}  // namespace

// ── Capture ─────────────────────────────────────────────────────────────────

class AudioCapture::Stream final : private StreamThread::Events {
public:
    Stream(const AudioCaptureOptions& options, Clock& clock, Sink sink, EndHandler ended)
        : format_(options.format),
          clock_(clock),
          sink_(std::move(sink)),
          sample_clock_(options.format.sample_rate),
          thread_(*this, std::move(ended)) {}

    Result<void> open(const AudioCaptureOptions& options) {
        uint8_t storage[512];
        spa_pod_builder b{};
        spa_pod_builder_init(&b, storage, sizeof storage);
        const spa_pod* params[] = {audio_format(b, format_)};
        pw_properties* properties = audio_properties("Capture", format_, options.latency);
        // What the default output plays: the monitor of its sink.
        if (options.node == 0 && options.system_sound)
            pw_properties_set(properties, PW_KEY_STREAM_CAPTURE_SINK, "true");
        StreamThread::Config config;
        config.node = options.node;
        config.name = "ratcast-audio-capture";
        config.direction = PW_DIRECTION_INPUT;
        config.flags = PW_STREAM_FLAG_RT_PROCESS;
        return thread_.open(config, properties, params);
    }

private:
    void on_process() override {
        // Every buffer, in order: sound, unlike pictures, cannot skip ahead.
        pw_stream* stream = thread_.stream();
        while (pw_buffer* buffer = pw_stream_dequeue_buffer(stream)) {
            std::optional<AudioFrame> block = copy(*buffer->buffer);
            pw_stream_queue_buffer(stream, buffer);
            if (block) sink_(std::move(*block));
        }
    }

    std::optional<AudioFrame> copy(const spa_buffer& buffer) {
        if (buffer.n_datas < 1) return std::nullopt;
        const spa_data& data = buffer.datas[0];
        if (!data.data || !data.chunk) return std::nullopt;
        const size_t offset = std::min<size_t>(data.chunk->offset, data.maxsize);
        const size_t bytes = std::min<size_t>(data.chunk->size, data.maxsize - offset);
        const size_t length = bytes / bytes_per_sample_time(format_);
        if (length == 0) return std::nullopt;

        MutableAudioFrame block = MutableAudioFrame::allocate(format_, length);
        std::memcpy(block.samples().data(), static_cast<const uint8_t*>(data.data) + offset,
                    length * bytes_per_sample_time(format_));
        return std::move(block).freeze(sample_clock_.stamp(length, clock_.now()));
    }

    const AudioFormat format_;
    Clock& clock_;
    const Sink sink_;
    SampleClock sample_clock_;  ///< on the data thread only

    StreamThread thread_;  ///< last: it stops calling back before the rest goes
};

AudioCapture::AudioCapture(const AudioCaptureOptions& options, Clock& clock)
    : options_(options), clock_(clock) {}

AudioCapture::~AudioCapture() { stop(); }

Result<void> AudioCapture::start(Sink sink, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (stream_) return fail(Errc::InvalidArgument);
    auto stream = std::make_unique<Stream>(options_, clock_, std::move(sink), std::move(ended));
    if (Result<void> opened = stream->open(options_); !opened) return opened;
    stream_ = std::move(stream);
    return {};
}

void AudioCapture::stop() {
    std::lock_guard lock(mutex_);
    stream_.reset();
}

// ── Playback ────────────────────────────────────────────────────────────────

class AudioPlayback::Stream final : private StreamThread::Events {
public:
    Stream(const AudioOutputOptions& options, Clock& clock, Pull pull, EndHandler ended)
        : format_(options.format),
          clock_(clock),
          pull_(std::move(pull)),
          thread_(*this, std::move(ended)) {}

    Result<void> open(const AudioOutputOptions& options) {
        uint8_t storage[512];
        spa_pod_builder b{};
        spa_pod_builder_init(&b, storage, sizeof storage);
        const spa_pod* params[] = {audio_format(b, format_)};
        StreamThread::Config config;
        config.node = options.node;
        config.name = "ratcast-audio-playback";
        config.direction = PW_DIRECTION_OUTPUT;
        config.flags = PW_STREAM_FLAG_RT_PROCESS;
        return thread_.open(config, audio_properties("Playback", format_, options.latency), params);
    }

    uint32_t node_id() const { return thread_.node_id(); }

private:
    void on_process() override {
        pw_stream* stream = thread_.stream();
        pw_buffer* buffer = pw_stream_dequeue_buffer(stream);
        if (!buffer) return;
        spa_data& data = buffer->buffer->datas[0];
        if (!data.data || !data.chunk) {
            pw_stream_queue_buffer(stream, buffer);
            return;
        }
        const size_t stride = bytes_per_sample_time(format_);
        size_t length = data.maxsize / stride;
        if (buffer->requested > 0) length = std::min<size_t>(length, buffer->requested);

        pull_({static_cast<float*>(data.data), length * format_.channels}, play_time(stream));
        data.chunk->offset = 0;
        data.chunk->stride = static_cast<int32_t>(stride);
        data.chunk->size = static_cast<uint32_t>(length * stride);
        pw_stream_queue_buffer(stream, buffer);
    }

    /// When the sound about to be queued reaches the speaker: after what the
    /// graph and the device still hold (delay) and what the converter holds
    /// (buffered).
    Timestamp play_time(pw_stream* stream) const {
        Timestamp at = clock_.now();
        pw_time time{};
        if (pw_stream_get_time_n(stream, &time, sizeof time) < 0) return at;
        if (time.rate.denom > 0 && time.delay > 0) {
            at += Duration{time.delay * 1'000'000 * time.rate.num / time.rate.denom};
        }
        return at + audio_duration(time.buffered, format_.sample_rate);
    }

    const AudioFormat format_;
    Clock& clock_;
    const Pull pull_;

    StreamThread thread_;  ///< last: it stops calling back before the rest goes
};

AudioPlayback::AudioPlayback(const AudioOutputOptions& options, Clock& clock)
    : options_(options), clock_(clock) {}

AudioPlayback::~AudioPlayback() { stop(); }

Result<void> AudioPlayback::start(Pull pull, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (stream_) return fail(Errc::InvalidArgument);
    auto stream = std::make_unique<Stream>(options_, clock_, std::move(pull), std::move(ended));
    if (Result<void> opened = stream->open(options_); !opened) return opened;
    stream_ = std::move(stream);
    return {};
}

void AudioPlayback::stop() {
    std::lock_guard lock(mutex_);
    stream_.reset();
}

uint32_t AudioPlayback::node_id() const {
    std::lock_guard lock(mutex_);
    return stream_ ? stream_->node_id() : 0;
}

}  // namespace ratcast::pipewire
