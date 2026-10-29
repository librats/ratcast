// Sound on Android through AAudio: see aaudio.h.

#include "android/aaudio.h"

#include <aaudio/AAudio.h>

#include <time.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "audio/sample_clock.h"

#include "librats/util/logger.h"

namespace ratcast::aaudio {
namespace {

/// When `nanos` of CLOCK_MONOTONIC — AAudio's stamps — was (or will be) on
/// `clock`, by how far it is from now.
Timestamp at_monotonic(Clock& clock, int64_t nanos) {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t now_nanos = int64_t(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
    return clock.now() + Duration{(nanos - now_nanos) / 1000};
}

/// The device `id` names (AudioDeviceInfo.getId()), or none for "" or what
/// is no such id: the default.
int32_t device_of(const std::string& id) {
    if (id.empty()) return AAUDIO_UNSPECIFIED;
    char* end = nullptr;
    const long device = std::strtol(id.c_str(), &end, 10);
    if (*end != '\0' || device <= 0) {
        LOG_WARN("engine", "sound device " << id << " is not there: the default in its place");
        return AAUDIO_UNSPECIFIED;
    }
    return int32_t(device);
}

/// What the stream is asked for.
struct Setup {
    aaudio_direction_t direction = AAUDIO_DIRECTION_OUTPUT;
    AudioFormat format;
    int32_t device = AAUDIO_UNSPECIFIED;
    Duration latency{};
};

/// One AAudio stream as `setup` asks, its data handed to `data` on AAudio's
/// realtime thread, opened again on a thread of its own when the default
/// device it plays to or hears changes. `ended` hears, once, from that thread,
/// when it stops by itself: success when a chosen device went.
class DeviceStream {
public:
    using Data = std::function<void(AAudioStream* stream, void* audio, int32_t frames)>;
    using Ended = std::function<void(Result<void>)>;

    DeviceStream(Setup setup, Data data, Ended ended)
        : setup_(setup), data_(std::move(data)), ended_(std::move(ended)) {}

    DeviceStream(const DeviceStream&) = delete;
    DeviceStream& operator=(const DeviceStream&) = delete;

    ~DeviceStream() {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
        close(stream_);
    }

    Result<void> start() {
        Result<AAudioStream*> opened = open();
        if (!opened) return fail(opened.error());
        stream_ = *opened;
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
        return {};
    }

private:
    Result<AAudioStream*> open() {
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return fail(Errc::Unavailable);
        const std::unique_ptr<AAudioStreamBuilder, decltype(&AAudioStreamBuilder_delete)> owned(
            builder, AAudioStreamBuilder_delete);
        const AudioFormat& format = setup_.format;
        AAudioStreamBuilder_setDirection(builder, setup_.direction);
        AAudioStreamBuilder_setDeviceId(builder, setup_.device);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, int32_t(format.channels));
        AAudioStreamBuilder_setSampleRate(builder, int32_t(format.sample_rate));
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(builder, on_data, this);
        AAudioStreamBuilder_setErrorCallback(builder, on_error, this);

        AAudioStream* stream = nullptr;
        aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
        if (result != AAUDIO_OK) {
            LOG_WARN("engine",
                     "cannot open the sound device: " << AAudio_convertResultToText(result));
            return fail(Errc::Unavailable);
        }
        // AAudio converts in its shared streams; one that could not is refused.
        if (AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_FLOAT ||
            AAudioStream_getChannelCount(stream) != int32_t(format.channels) ||
            AAudioStream_getSampleRate(stream) != int32_t(format.sample_rate)) {
            LOG_WARN("engine", "the sound device takes no " << format.sample_rate << " Hz, "
                                                            << format.channels << " channels");
            close(stream);
            return fail(Errc::Unavailable);
        }
        if (setup_.direction == AAUDIO_DIRECTION_OUTPUT) {
            // Played as far ahead as the latency asked for, in whole bursts,
            // two at least: one played while the next is filled.
            const int32_t burst = std::max(1, AAudioStream_getFramesPerBurst(stream));
            const int32_t asked = int32_t(audio_length(setup_.latency, format.sample_rate));
            AAudioStream_setBufferSizeInFrames(stream,
                                               std::max(2, (asked + burst - 1) / burst) * burst);
        }
        result = AAudioStream_requestStart(stream);
        if (result != AAUDIO_OK) {
            LOG_WARN("engine",
                     "cannot start the sound device: " << AAudio_convertResultToText(result));
            close(stream);
            return fail(Errc::Unavailable);
        }
        return stream;
    }

    /// Stopped before it is closed: AAudio's callbacks are over once it has.
    static void close(AAudioStream* stream) {
        if (!stream) return;
        if (AAudioStream_requestStop(stream) == AAUDIO_OK) {
            aaudio_stream_state_t state = AAUDIO_STREAM_STATE_STOPPING;
            AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_STOPPING, &state,
                                            100'000'000);
        }
        AAudioStream_close(stream);
    }

    /// The stream's own thread: what its errors ask for, as nothing may close
    /// a stream from its callbacks.
    void run(std::stop_token stop) {
        for (;;) {
            aaudio_result_t error = AAUDIO_OK;
            {
                std::unique_lock lock(mutex_);
                if (!changed_.wait(lock, stop, [this] { return error_ != AAUDIO_OK; })) return;
                error = std::exchange(error_, AAUDIO_OK);
            }
            close(stream_);
            stream_ = nullptr;
            Result<void> outcome;
            if (error != AAUDIO_ERROR_DISCONNECTED) {
                LOG_WARN("engine",
                         "the sound device failed: " << AAudio_convertResultToText(error));
                outcome = fail(Errc::Internal);
            } else if (setup_.device == AAUDIO_UNSPECIFIED) {
                // The default changed: the new one opened in its place.
                Result<AAudioStream*> opened = open();
                if (opened) {
                    stream_ = *opened;
                    continue;
                }
                outcome = fail(opened.error());
            }
            if (ended_) ended_(std::move(outcome));
            return;
        }
    }

    static aaudio_data_callback_result_t on_data(AAudioStream* stream, void* user, void* audio,
                                                 int32_t frames) {
        static_cast<DeviceStream*>(user)->data_(stream, audio, frames);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    /// On a thread of AAudio's.
    static void on_error(AAudioStream*, void* user, aaudio_result_t error) {
        auto* self = static_cast<DeviceStream*>(user);
        {
            std::lock_guard lock(self->mutex_);
            if (self->error_ == AAUDIO_OK) self->error_ = error;
        }
        self->changed_.notify_one();
    }

    const Setup setup_;
    const Data data_;
    const Ended ended_;
    AAudioStream* stream_ = nullptr;  ///< start()'s, then the worker's

    std::mutex mutex_;
    std::condition_variable_any changed_;
    aaudio_result_t error_ = AAUDIO_OK;  ///< the first since the stream opened

    std::jthread worker_;  ///< last: stopped first
};

}  // namespace

// ── Capture ─────────────────────────────────────────────────────────────────

class AudioCapture::Stream {
public:
    Stream(const AudioCaptureOptions& options, Clock& clock, Sink sink, EndHandler ended)
        : format_(options.format),
          clock_(clock),
          sink_(std::move(sink)),
          sample_clock_(options.format.sample_rate),
          device_(
              {AAUDIO_DIRECTION_INPUT, options.format, device_of(options.device), options.latency},
              [this](AAudioStream* stream, void* audio, int32_t frames) {
                  captured(stream, static_cast<const float*>(audio), frames);
              },
              std::move(ended)) {}

    Result<void> start() { return device_.start(); }

private:
    /// On AAudio's realtime thread.
    void captured(AAudioStream* stream, const float* audio, int32_t frames) {
        if (frames <= 0) return;
        if (stream != stream_) {  // opened again: its count starts afresh
            stream_ = stream;
            sample_clock_.reset();
        }
        // Complete when its last frame came in: the stream's stamp of a
        // frame it counts, as far on from it as that frame is. The frames
        // read so far are this block's first.
        Timestamp complete = clock_.now();
        int64_t position = 0;
        int64_t nanos = 0;
        if (AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position, &nanos) == AAUDIO_OK) {
            const int64_t last = AAudioStream_getFramesRead(stream) + frames;
            complete = at_monotonic(
                clock_, nanos + (last - position) * 1'000'000'000 / int64_t(format_.sample_rate));
        }
        const size_t length = size_t(frames);
        MutableAudioFrame block = MutableAudioFrame::allocate(format_, length);
        std::memcpy(block.samples().data(), audio, length * format_.channels * sizeof(float));
        sink_(std::move(block).freeze(sample_clock_.stamp(length, complete)));
    }

    const AudioFormat format_;
    Clock& clock_;
    const Sink sink_;
    // The realtime thread's alone.
    SampleClock sample_clock_;
    AAudioStream* stream_ = nullptr;

    DeviceStream device_;  ///< last: closed first
};

AudioCapture::AudioCapture(const AudioCaptureOptions& options, Clock& clock)
    : options_(options), clock_(clock) {}

AudioCapture::~AudioCapture() { stop(); }

Result<void> AudioCapture::start(Sink sink, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (stream_) return fail(Errc::InvalidArgument);
    if (options_.source != AudioCaptureSource::Microphone || options_.node != 0)
        return fail(Errc::Unavailable);
    auto stream = std::make_unique<Stream>(options_, clock_, std::move(sink), std::move(ended));
    if (Result<void> started = stream->start(); !started) return started;
    stream_ = std::move(stream);
    return {};
}

void AudioCapture::stop() {
    std::lock_guard lock(mutex_);
    stream_.reset();
}

// ── Playback ────────────────────────────────────────────────────────────────

class AudioPlayback::Stream {
public:
    Stream(const AudioOutputOptions& options, Clock& clock, Pull pull, EndHandler ended)
        : format_(options.format),
          clock_(clock),
          pull_(std::move(pull)),
          device_(
              {AAUDIO_DIRECTION_OUTPUT, options.format, device_of(options.device), options.latency},
              [this](AAudioStream* stream, void* audio, int32_t frames) {
                  play(stream, static_cast<float*>(audio), frames);
              },
              std::move(ended)) {}

    Result<void> start() { return device_.start(); }

private:
    /// On AAudio's realtime thread.
    void play(AAudioStream* stream, float* audio, int32_t frames) {
        if (frames <= 0) return;
        // Heard when the stream's stamp says, as far on from the frame it
        // stamps as this pull's first is: the frames written so far. Before
        // the stream has one, after what is queued.
        const int64_t first = AAudioStream_getFramesWritten(stream);
        Timestamp at;
        int64_t position = 0;
        int64_t nanos = 0;
        if (AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position, &nanos) == AAUDIO_OK) {
            at = at_monotonic(
                clock_, nanos + (first - position) * 1'000'000'000 / int64_t(format_.sample_rate));
        } else {
            const int64_t queued = std::max<int64_t>(0, first - AAudioStream_getFramesRead(stream));
            at = clock_.now() + audio_duration(size_t(queued), format_.sample_rate);
        }
        pull_({audio, size_t(frames) * format_.channels}, at);
    }

    const AudioFormat format_;
    Clock& clock_;
    const Pull pull_;

    DeviceStream device_;  ///< last: closed first
};

AudioPlayback::AudioPlayback(const AudioOutputOptions& options, Clock& clock)
    : options_(options), clock_(clock) {}

AudioPlayback::~AudioPlayback() { stop(); }

Result<void> AudioPlayback::start(Pull pull, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (stream_) return fail(Errc::InvalidArgument);
    if (options_.node != 0) return fail(Errc::Unavailable);
    auto stream = std::make_unique<Stream>(options_, clock_, std::move(pull), std::move(ended));
    if (Result<void> started = stream->start(); !started) return started;
    stream_ = std::move(stream);
    return {};
}

void AudioPlayback::stop() {
    std::lock_guard lock(mutex_);
    stream_.reset();
}

// ── Devices ─────────────────────────────────────────────────────────────────

Result<std::vector<AudioDeviceInfo>> list_devices(bool) { return std::vector<AudioDeviceInfo>{}; }

}  // namespace ratcast::aaudio
