// Both streams run on a thread of their own in COM's multithreaded apartment,
// woken by the device's event. The stream is activated there too, and start()
// waits for that to say whether it worked: the audio client then lives and
// dies on the one thread.

#include "windows/wasapi_audio.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "audio/sample_clock.h"
#include "windows/com.h"
#include "windows/qpc.h"

#include "librats/util/logger.h"

namespace ratcast::wasapi {
namespace {

using namespace std::chrono_literals;
using win::ComPtr;
using win::from_hundreds_of_ns;
using win::qpc_now;

// The default devices as interface paths (DEVINTERFACE_AUDIO_CAPTURE and
// _RENDER): a stream activated by one follows the default as it changes.
constexpr wchar_t kDefaultCapture[] = L"{2EEF81BE-33FA-4800-9670-1CD474972C3F}";
constexpr wchar_t kDefaultRender[] = L"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}";

/// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT.
constexpr GUID kFloatSamples = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr DWORD kActivationPatienceMs = 5000;
/// How long a device period is when the device does not say.
constexpr Duration kDefaultPeriod = 10ms;
/// How long a stream waits on its device before looking again regardless.
constexpr DWORD kIdleWaitMs = 200;

WAVEFORMATEXTENSIBLE wave_format(const AudioFormat& format) {
    WAVEFORMATEXTENSIBLE wave{};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = static_cast<WORD>(format.channels);
    wave.Format.nSamplesPerSec = format.sample_rate;
    wave.Format.wBitsPerSample = 32;
    wave.Format.nBlockAlign = static_cast<WORD>(sizeof(float) * format.channels);
    wave.Format.nAvgBytesPerSec = format.sample_rate * wave.Format.nBlockAlign;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wave.Samples.wValidBitsPerSample = 32;
    wave.dwChannelMask = format.channels == 1   ? SPEAKER_FRONT_CENTER
                         : format.channels == 2 ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
                                                : 0;
    wave.SubFormat = kFloatSamples;
    return wave;
}

/// Hears how an activation came out. Agile (free-threaded), as the system
/// calls it on a thread of its own.
class Activation final : public Microsoft::WRL::RuntimeClass<
                             Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                             Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    Activation() : done_(win::make_event(true)) {}

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activated = E_FAIL;
        ComPtr<IUnknown> unknown;
        const HRESULT asked = operation->GetActivateResult(&activated, &unknown);
        result_ = FAILED(asked) ? asked : activated;
        if (SUCCEEDED(result_)) result_ = unknown.As(&client_);
        SetEvent(done_.get());
        return S_OK;
    }

    /// The client, once the activation is complete.
    Result<ComPtr<IAudioClient>> wait() {
        if (WaitForSingleObject(done_.get(), kActivationPatienceMs) != WAIT_OBJECT_0)
            return fail(Errc::Unavailable);
        if (FAILED(result_)) return fail(win::errc_of(result_));
        return client_;
    }

    HANDLE done() const noexcept { return done_.get(); }

private:
    win::Handle done_;
    HRESULT result_ = E_FAIL;
    ComPtr<IAudioClient> client_;
};

/// An audio client for the device at `path`, `params` as the activation's.
Result<ComPtr<IAudioClient>> activate(const wchar_t* path, PROPVARIANT* params) {
    ComPtr<Activation> activation = Microsoft::WRL::Make<Activation>();
    if (!activation || !activation->done()) return fail(Errc::Internal);
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    const HRESULT result = ActivateAudioInterfaceAsync(path, __uuidof(IAudioClient), params,
                                                       activation.Get(), &operation);
    if (FAILED(result)) return fail(win::errc_of(result));
    Result<ComPtr<IAudioClient>> client = activation->wait();
    if (!client) LOG_WARN("engine", "cannot open the sound device: " << to_string(client.error()));
    return client;
}

/// An audio client on the endpoint of `id` (IMMDevice::GetId's), if it is
/// there and on.
Result<ComPtr<IAudioClient>> activate_endpoint(const std::string& id) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
    ComPtr<IMMDevice> device;
    if (SUCCEEDED(result)) result = enumerator->GetDevice(win::wide(id).c_str(), &device);
    DWORD state = 0;
    if (SUCCEEDED(result)) result = device->GetState(&state);
    if (SUCCEEDED(result) && state != DEVICE_STATE_ACTIVE)
        result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    ComPtr<IAudioClient> client;
    if (SUCCEEDED(result))
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(result)) return fail(win::errc_of(result));
    return client;
}

/// An audio client on the device of `id`, or — none chosen, or not there —
/// on the default at `default_path`, which it follows as that changes.
Result<ComPtr<IAudioClient>> activate_device(const std::string& id, const wchar_t* default_path) {
    if (!id.empty()) {
        Result<ComPtr<IAudioClient>> client = activate_endpoint(id);
        if (client) return client;
        LOG_WARN("engine", "sound device " << id << " is not there: the default in its place");
    }
    return activate(default_path, nullptr);
}

/// PKEY_Device_FriendlyName, which would otherwise need its own library.
constexpr PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

/// A loopback of one process's sound and its children's — or, excluding, of
/// everything but theirs.
Result<ComPtr<IAudioClient>> activate_process_loopback(DWORD process, bool excluding) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = process;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        excluding ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
                  : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT variant{};
    variant.vt = VT_BLOB;
    variant.blob.cbSize = sizeof params;
    variant.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    return activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, &variant);
}

/// Set `client` up for `format`, converted to and from the device's, waking
/// `ready` as the device has room or sound.
Result<void> initialize(IAudioClient& client, const AudioFormat& format, DWORD flags,
                        Duration buffer, HANDLE ready) {
    const WAVEFORMATEXTENSIBLE wave = wave_format(format);
    flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
             AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HRESULT result = client.Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer.count() * 10, 0,
                                       &wave.Format, nullptr);
    if (SUCCEEDED(result)) result = client.SetEventHandle(ready);
    if (FAILED(result)) {
        LOG_WARN("engine", "cannot set the sound device up: HRESULT " << std::hex << result);
        return fail(win::errc_of(result));
    }
    return {};
}

/// The device's period: how often it wakes the stream.
Duration device_period(IAudioClient& client) {
    REFERENCE_TIME period = 0;
    if (FAILED(client.GetDevicePeriod(&period, nullptr)) || period <= 0) return kDefaultPeriod;
    return from_hundreds_of_ns(period);
}

/// The thread a stream runs on, from start() to stop(): open() sets the
/// stream up there, and serve() serves it until it ends by itself (how) or
/// is stopped (nullopt). close() lets go of it, on the same thread.
class Worker {
public:
    using EndHandler = std::function<void(Result<void> outcome)>;

    explicit Worker(EndHandler ended) : ended_(std::move(ended)), stop_(win::make_event(true)) {}
    virtual ~Worker() = default;  // a derived class stops in its own destructor

    /// What open() said; serving if it worked.
    Result<void> start() {
        if (!stop_) return fail(Errc::Internal);
        std::promise<Result<void>> opened;
        std::future<Result<void>> outcome = opened.get_future();
        thread_ = std::thread([this, opened = std::move(opened)]() mutable {
            const win::ComApartment com;
            const Result<void> ready = com.ok() ? open() : fail(Errc::Internal);
            opened.set_value(ready);
            if (!ready) {
                close();
                return;
            }
            DWORD task = 0;
            const HANDLE scheduling = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
            const std::optional<Result<void>> ended = serve();
            if (scheduling) AvRevertMmThreadCharacteristics(scheduling);
            close();
            if (ended && ended_) ended_(*ended);
        });
        Result<void> ready = outcome.get();
        if (!ready) thread_.join();
        return ready;
    }

    void stop() {
        SetEvent(stop_.get());
        if (thread_.joinable()) thread_.join();
    }

protected:
    virtual Result<void> open() = 0;
    virtual std::optional<Result<void>> serve() = 0;
    virtual void close() = 0;

    HANDLE stop_event() const noexcept { return stop_.get(); }

private:
    const EndHandler ended_;
    win::Handle stop_;
    std::thread thread_;
};

}  // namespace

// ── Capture ─────────────────────────────────────────────────────────────────

class AudioCapture::Stream final : public Worker {
public:
    Stream(const AudioCaptureOptions& options, Clock& clock, Sink sink, EndHandler ended)
        : Worker(std::move(ended)),
          options_(options),
          clock_(clock),
          sink_(std::move(sink)),
          sample_clock_(options.format.sample_rate),
          ready_(win::make_event()) {}
    ~Stream() override { stop(); }

private:
    /// What the computer plays, rather than a microphone.
    bool loopback() const noexcept {
        return options_.node != 0 || options_.source != AudioCaptureSource::Microphone;
    }

    Result<void> open() override {
        if (!ready_) return fail(Errc::Internal);
        if (options_.node != 0) {
            process_.reset(OpenProcess(SYNCHRONIZE, FALSE, options_.node));
            if (!process_) return {};  // serve() says it is not there
        }
        Result<ComPtr<IAudioClient>> client = activate_client();
        if (!client) return fail(client.error());
        client_ = std::move(*client);
        period_ = device_period(*client_.Get());
        const Duration buffer = std::max<Duration>(options_.latency * 4, 40ms);
        if (Result<void> set =
                initialize(*client_.Get(), options_.format,
                           loopback() ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, buffer, ready_.get());
            !set) {
            return set;
        }
        HRESULT result = client_->GetService(IID_PPV_ARGS(&capture_));
        if (SUCCEEDED(result)) result = client_->Start();
        if (FAILED(result)) return fail(win::errc_of(result));
        return {};
    }

    Result<ComPtr<IAudioClient>> activate_client() const {
        if (options_.node != 0) return activate_process_loopback(options_.node, false);
        switch (options_.source) {
            case AudioCaptureSource::Microphone:
                return activate_device(options_.device, kDefaultCapture);
            case AudioCaptureSource::SystemSound:
                return activate_device(options_.device, kDefaultRender);
            case AudioCaptureSource::Applications:
                return activate_process_loopback(options_.excluded_process != 0
                                                     ? DWORD(options_.excluded_process)
                                                     : GetCurrentProcessId(),
                                                 true);
        }
        return fail(Errc::InvalidArgument);
    }

    std::optional<Result<void>> serve() override {
        if (!client_) {
            LOG_WARN("engine", "process " << options_.node << " does not exist");
            return fail(Errc::Unavailable);
        }
        const HANDLE events[] = {stop_event(), ready_.get(), process_.get()};
        const DWORD count = process_ ? 3 : 2;
        // A loopback is looked at each period, to fill its silences in.
        const DWORD wait_ms =
            loopback() ? DWORD(std::max<int64_t>(period_.count() / 1000, 1)) : kIdleWaitMs;
        filled_until_ = clock_.now();
        for (;;) {
            const DWORD woke = WaitForMultipleObjects(count, events, FALSE, wait_ms);
            if (woke == WAIT_OBJECT_0) return std::nullopt;
            if (woke == WAIT_OBJECT_0 + 2) return Result<void>{};  // the process exited
            if (woke == WAIT_FAILED) return fail(Errc::Internal);
            const HRESULT result = drain();
            // Following the default, this means there is none left.
            if (result == AUDCLNT_E_DEVICE_INVALIDATED) return Result<void>{};
            if (FAILED(result)) return fail(win::errc_of(result));
            if (loopback()) fill_silence();
        }
    }

    void close() override {
        if (client_) client_->Stop();
        capture_.Reset();
        client_.Reset();
    }

    /// Every packet the device holds, in order: sound, unlike pictures,
    /// cannot skip ahead.
    HRESULT drain() {
        const AudioFormat& format = options_.format;
        for (;;) {
            UINT32 packet = 0;
            HRESULT result = capture_->GetNextPacketSize(&packet);
            if (FAILED(result) || packet == 0) return result;
            BYTE* data = nullptr;
            UINT32 length = 0;
            DWORD flags = 0;
            UINT64 qpc = 0;
            result = capture_->GetBuffer(&data, &length, &flags, nullptr, &qpc);
            if (FAILED(result)) return result;
            if (result == AUDCLNT_S_BUFFER_EMPTY || length == 0) {
                capture_->ReleaseBuffer(length);
                return S_OK;
            }

            MutableAudioFrame block = MutableAudioFrame::allocate(format, length);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(block.samples().begin(), block.samples().end(), 0.0f);
            } else {
                std::memcpy(block.samples().data(), data, block.samples().size_bytes());
            }
            capture_->ReleaseBuffer(length);
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) sample_clock_.reset();

            // Complete when its last sample was captured: by the device's
            // stamp on its first, where it gives one.
            Timestamp complete = clock_.now();
            if (qpc != 0 && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                complete -= from_hundreds_of_ns(qpc_now() - int64_t(qpc));
                complete += audio_duration(length, format.sample_rate);
            }
            deliver(std::move(block).freeze(sample_clock_.stamp(length, complete)));
        }
    }

    /// A loopback hears nothing while nothing plays: silence in its place,
    /// once the device has been quiet for longer than it is between packets.
    void fill_silence() {
        const Timestamp now = clock_.now();
        if (now - filled_until_ < 3 * period_) return;
        const size_t length = audio_length(now - filled_until_, options_.format.sample_rate);
        if (length == 0) return;
        MutableAudioFrame block = MutableAudioFrame::allocate(options_.format, length);
        std::fill(block.samples().begin(), block.samples().end(), 0.0f);
        const Timestamp complete =
            filled_until_ + audio_duration(length, options_.format.sample_rate);
        deliver(std::move(block).freeze(sample_clock_.stamp(length, complete)));
    }

    void deliver(AudioFrame block) {
        filled_until_ = block.capture_time() + block.duration();
        sink_(std::move(block));
    }

    const AudioCaptureOptions options_;
    Clock& clock_;
    const Sink sink_;
    SampleClock sample_clock_;
    win::Handle ready_;
    win::Handle process_;  ///< the node's, which the capture ends with

    // The stream's thread's alone.
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    Duration period_ = kDefaultPeriod;
    Timestamp filled_until_{};  ///< where the blocks handed out so far end
};

AudioCapture::AudioCapture(const AudioCaptureOptions& options, Clock& clock)
    : options_(options), clock_(clock) {}

AudioCapture::~AudioCapture() { stop(); }

Result<void> AudioCapture::start(Sink sink, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (stream_) return fail(Errc::InvalidArgument);
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

class AudioPlayback::Stream final : public Worker {
public:
    Stream(const AudioOutputOptions& options, Clock& clock, Pull pull, EndHandler ended)
        : Worker(std::move(ended)),
          options_(options),
          clock_(clock),
          pull_(std::move(pull)),
          ready_(win::make_event()) {}
    ~Stream() override { stop(); }

private:
    Result<void> open() override {
        if (!ready_) return fail(Errc::Internal);
        if (options_.node != 0) return fail(Errc::Unavailable);
        Result<ComPtr<IAudioClient>> client = activate_device(options_.device, kDefaultRender);
        if (!client) return fail(client.error());
        client_ = std::move(*client);
        const Duration buffer = std::max<Duration>(options_.latency * 2, 20ms);
        if (Result<void> set = initialize(*client_.Get(), options_.format, 0, buffer, ready_.get());
            !set) {
            return set;
        }
        HRESULT result = client_->GetBufferSize(&buffer_length_);
        if (SUCCEEDED(result)) result = client_->GetService(IID_PPV_ARGS(&render_));
        // A period at least: the audio engine takes the buffer a period at a
        // time. Some streams say no more (with conversion: nothing at all).
        REFERENCE_TIME latency = 0;
        stream_latency_ = device_period(*client_.Get());
        if (SUCCEEDED(client_->GetStreamLatency(&latency))) {
            stream_latency_ = std::max(stream_latency_, from_hundreds_of_ns(latency));
        }
        if (FAILED(result)) return fail(win::errc_of(result));
        // Without it, play times come from what is queued alone.
        if (SUCCEEDED(client_->GetService(IID_PPV_ARGS(&audio_clock_))) &&
            FAILED(audio_clock_->GetFrequency(&clock_frequency_))) {
            audio_clock_.Reset();
        }
        // The first sound in place before the device starts asking.
        if (result = fill(); SUCCEEDED(result)) result = client_->Start();
        if (FAILED(result)) return fail(win::errc_of(result));
        return {};
    }

    std::optional<Result<void>> serve() override {
        const HANDLE events[] = {stop_event(), ready_.get()};
        for (;;) {
            const DWORD woke = WaitForMultipleObjects(2, events, FALSE, kIdleWaitMs);
            if (woke == WAIT_OBJECT_0) return std::nullopt;
            if (woke == WAIT_FAILED) return fail(Errc::Internal);
            const HRESULT result = fill();
            if (result == AUDCLNT_E_DEVICE_INVALIDATED) return Result<void>{};
            if (FAILED(result)) return fail(win::errc_of(result));
        }
    }

    void close() override {
        if (client_) client_->Stop();
        audio_clock_.Reset();
        render_.Reset();
        client_.Reset();
    }

    /// As much as the device has room for.
    HRESULT fill() {
        UINT32 queued = 0;
        HRESULT result = client_->GetCurrentPadding(&queued);
        if (FAILED(result)) return result;
        const UINT32 length = buffer_length_ - std::min(queued, buffer_length_);
        if (length == 0) return S_OK;
        BYTE* data = nullptr;
        result = render_->GetBuffer(length, &data);
        if (FAILED(result)) return result;
        const Timestamp at = play_time(queued);
        pull_({reinterpret_cast<float*>(data), size_t{length} * options_.format.channels}, at);
        written_ += length;
        return render_->ReleaseBuffer(length, 0);
    }

    /// When the sound about to be written is heard: after all written before
    /// it, by the device's clock of what it is playing now — and never before
    /// what is still queued has gone through the audio engine.
    Timestamp play_time(UINT32 queued) const {
        const Timestamp now = clock_.now();
        const uint32_t rate = options_.format.sample_rate;
        const Timestamp earliest = now + audio_duration(queued, rate) + stream_latency_;
        UINT64 position = 0;
        UINT64 qpc = 0;
        if (audio_clock_ && SUCCEEDED(audio_clock_->GetPosition(&position, &qpc)) && qpc != 0) {
            const Duration ahead =
                audio_duration(written_, rate) -
                Duration{int64_t(position * 1'000'000 / std::max<UINT64>(clock_frequency_, 1))};
            const Timestamp at = now - from_hundreds_of_ns(qpc_now() - int64_t(qpc)) + ahead;
            return std::max(at, earliest);
        }
        return earliest;
    }

    const AudioOutputOptions options_;
    Clock& clock_;
    const Pull pull_;
    win::Handle ready_;

    // The stream's thread's alone.
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    ComPtr<IAudioClock> audio_clock_;
    UINT64 clock_frequency_ = 0;
    UINT32 buffer_length_ = 0;
    Duration stream_latency_{};  ///< the audio engine's, past the buffer: the least there is
    uint64_t written_ = 0;       ///< samples per channel handed to the device
};

AudioPlayback::AudioPlayback(const AudioOutputOptions& options, Clock& clock)
    : options_(options), clock_(clock) {}

AudioPlayback::~AudioPlayback() { stop(); }

Result<void> AudioPlayback::start(Pull pull, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (stream_) return fail(Errc::InvalidArgument);
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

Result<std::vector<AudioDeviceInfo>> list_devices(bool capture) {
    const win::ComApartment com;
    if (!com.ok()) return fail(Errc::Unavailable);
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
    ComPtr<IMMDeviceCollection> endpoints;
    if (SUCCEEDED(result))
        result = enumerator->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE,
                                                &endpoints);
    UINT count = 0;
    if (SUCCEEDED(result)) result = endpoints->GetCount(&count);
    if (FAILED(result)) return fail(win::errc_of(result));
    std::vector<AudioDeviceInfo> devices;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        LPWSTR id = nullptr;
        if (FAILED(endpoints->Item(i, &device)) || FAILED(device->GetId(&id))) continue;
        AudioDeviceInfo info{.id = win::utf8(id)};
        CoTaskMemFree(id);
        ComPtr<IPropertyStore> properties;
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
            SUCCEEDED(properties->GetValue(kFriendlyName, &name)) && name.vt == VT_LPWSTR) {
            info.name = win::utf8(name.pwszVal);
        }
        PropVariantClear(&name);
        if (info.name.empty()) info.name = capture ? "Microphone" : "Speakers";
        devices.push_back(std::move(info));
    }
    return devices;
}

}  // namespace ratcast::wasapi
