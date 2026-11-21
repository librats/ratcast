#include "windows/desktop_duplication.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <windows.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "capture/frame_pacer.h"
#include "windows/com.h"
#include "windows/d3d11.h"
#include "windows/input_desktop.h"
#include "windows/qpc.h"
#include "windows/texture_reader.h"

#include "librats/util/logger.h"

namespace ratcast::dxgi {
namespace {

using namespace std::chrono_literals;
using win::ComPtr;

constexpr std::string_view kMonitor = "monitor:";
/// How long a wait for the screen's next change lasts: as long as stop() may
/// be kept waiting.
constexpr UINT kWaitMs = 100;
/// How long before trying again what failed (the secure desktop, a mode change).
constexpr auto kRetry = 250ms;
/// How long the screen may be missing (a mode change takes it away a moment)
/// before the capture ends, as Windows.Graphics.Capture's does when its screen
/// goes.
constexpr auto kGone = 5s;

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(std::max(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
    return out;
}

/// The output whose device is `name` ("\\.\DISPLAY1"), and its adapter.
struct Output {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
};

std::optional<Output> find_output(const std::wstring& name) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return std::nullopt;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && _wcsicmp(desc.DeviceName, name.c_str()) == 0) {
                Output found{adapter, nullptr};
                if (FAILED(output.As(&found.output))) return std::nullopt;
                return found;
            }
            output.Reset();
        }
        adapter.Reset();
    }
    return std::nullopt;
}

/// The duplication of `output` on `device`: BGRA whatever the desktop is
/// drawn in (an HDR one too) where the system can say so.
HRESULT duplicate(IDXGIOutput1& output, ID3D11Device& device,
                  ComPtr<IDXGIOutputDuplication>& duplication) {
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output.QueryInterface(IID_PPV_ARGS(&output5)))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        const HRESULT result =
            output5->DuplicateOutput1(&device, 0, UINT(std::size(formats)), formats, &duplication);
        if (SUCCEEDED(result)) return result;
        // Not per-monitor DPI aware: the older call, which is the same then.
    }
    return output.DuplicateOutput(&device, &duplication);
}

/// The performance counter's ticks in 100 ns units, as qpc.h counts.
int64_t hundreds_of_ns(int64_t ticks) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return ticks / frequency.QuadPart * 10'000'000 +
           ticks % frequency.QuadPart * 10'000'000 / frequency.QuadPart;
}

class Duplication final : public ScreenCapture {
public:
    Duplication(std::wstring device_name, std::string token, const ScreenCaptureOptions& options,
                Clock& clock)
        : device_name_(std::move(device_name)),
          token_(std::move(token)),
          options_(options),
          clock_(clock) {}
    ~Duplication() override { stop(); }

    Result<void> start(Sink sink, EndHandler ended) override {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) return fail(Errc::InvalidArgument);
        thread_ = std::jthread([this, sink = std::move(sink), ended = std::move(ended)](
                                   std::stop_token stop) { run(stop, sink, ended); });
        return {};
    }

    void stop() override {
        std::lock_guard lock(mutex_);
        if (!thread_.joinable()) return;
        thread_.request_stop();
        thread_.join();
        thread_ = {};
    }

    const std::string& restore_token() const noexcept override { return token_; }

private:
    /// What a run holds: the device of the output's adapter and the
    /// duplication on it.
    struct Session {
        std::optional<Output> output;
        LUID adapter{};
        std::optional<d3d11::TextureReader> reader;
        ComPtr<IDXGIOutputDuplication> duplication;
        DXGI_OUTDUPL_DESC desc{};
        ComPtr<ID3D11Texture2D> held;  ///< a change the frame rate held back
        Timestamp held_at{};
    };

    /// The screen duplicated afresh, on its adapter's device (made again if
    /// the screen moved to another adapter).
    HRESULT open(Session& session) {
        session.duplication.Reset();
        session.held.Reset();
        session.output = find_output(device_name_);
        if (!session.output) return DXGI_ERROR_NOT_FOUND;
        DXGI_ADAPTER_DESC1 adapter{};
        session.output->adapter->GetDesc1(&adapter);
        const bool same = session.reader &&
                          adapter.AdapterLuid.LowPart == session.adapter.LowPart &&
                          adapter.AdapterLuid.HighPart == session.adapter.HighPart;
        if (!same) {
            session.reader.reset();
            Result<std::shared_ptr<d3d11::Device>> device =
                d3d11::Device::create(session.output->adapter.Get());
            if (!device) return E_FAIL;
            session.reader.emplace(std::move(*device), options_.format, options_.color_space,
                                   options_.gpu);
            session.adapter = adapter.AdapterLuid;
        }
        const HRESULT result = duplicate(*session.output->output.Get(),
                                         *session.reader->device()->get(), session.duplication);
        if (SUCCEEDED(result)) session.duplication->GetDesc(&session.desc);
        return result;
    }

    void run(std::stop_token stop, const Sink& sink, const EndHandler& ended) {
        // Physical pixels, as the screen's own.
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        win::InputDesktop desktop;
        Session session;
        FramePacer pacer(options_.max_frame_rate);
        HRESULT last_failure = S_OK;
        std::optional<std::chrono::steady_clock::time_point> missing_since;
        while (!stop.stop_requested()) {
            if (!session.duplication) {
                desktop.follow();
                const HRESULT opened = open(session);
                if (opened == DXGI_ERROR_NOT_FOUND) {
                    // The screen unplugged, or gone a moment in a mode change:
                    // the capture over once it has stayed away.
                    const auto now = std::chrono::steady_clock::now();
                    if (!missing_since) missing_since = now;
                    if (now - *missing_since >= kGone) {
                        LOG_INFO("engine", "screen duplication ends: the screen is gone");
                        if (ended) ended(Result<void>{});
                        return;
                    }
                } else {
                    missing_since.reset();
                }
                if (FAILED(opened)) {
                    if (opened != last_failure) {
                        LOG_INFO("engine",
                                 "screen duplication waits: 0x" << std::hex << unsigned(opened));
                        last_failure = opened;
                    }
                    std::this_thread::sleep_for(kRetry);
                    continue;
                }
                if (last_failure != S_OK) LOG_INFO("engine", "screen duplication goes on");
                last_failure = S_OK;
            }

            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> resource;
            // While a change is held back, only until its time may come.
            const HRESULT acquired =
                session.duplication->AcquireNextFrame(session.held ? 4 : kWaitMs, &info, &resource);
            if (acquired != DXGI_ERROR_WAIT_TIMEOUT) {
                if (FAILED(acquired)) {
                    // The desktop switched, the mode changed, a full-screen
                    // application took the screen: duplicated again, wherever
                    // input is now.
                    session.duplication.Reset();
                    continue;
                }
                if (info.LastPresentTime.QuadPart != 0) {  // else the pointer alone moved
                    ComPtr<ID3D11Texture2D> texture;
                    resource.As(&texture);
                    const Timestamp captured =
                        std::min(win::at_qpc(hundreds_of_ns(info.LastPresentTime.QuadPart), clock_),
                                 clock_.now());
                    if (texture && pacer.admit(clock_.now())) {
                        session.held.Reset();
                        give(session, *texture.Get(), captured, sink);
                    } else if (texture) {
                        hold(session, *texture.Get(), captured);
                    }
                }
                if (session.duplication) session.duplication->ReleaseFrame();
            }
            // A change held back goes when its time comes, whatever woke the
            // loop: not only a wait that timed out, which a pointer moving
            // all the while on a fast screen (a frame of its own each
            // refresh, at 240 Hz and up quicker than the wait) never lets
            // happen.
            if (session.held && session.reader && pacer.admit(clock_.now())) {
                give(session, *session.held.Get(), session.held_at, sink);
                session.held.Reset();
            }
        }
    }

    /// The screen as it is in `texture`, to the sink.
    void give(Session& session, ID3D11Texture2D& texture, Timestamp captured, const Sink& sink) {
        Result<std::optional<VideoFrame>> picture = session.reader->read(
            texture, session.desc.ModeDesc.Width, session.desc.ModeDesc.Height, captured);
        if (!picture) {
            session.duplication.Reset();  // the GPU went: everything made again
            session.reader.reset();
            return;
        }
        if (*picture) sink(std::move(**picture));
    }

    /// A change kept to send when the frame rate lets it: the duplication's
    /// texture is the system's only until the frame is released.
    void hold(Session& session, ID3D11Texture2D& texture, Timestamp captured) {
        D3D11_TEXTURE2D_DESC desc;
        texture.GetDesc(&desc);
        D3D11_TEXTURE2D_DESC held_desc{};
        if (session.held) session.held->GetDesc(&held_desc);
        if (!session.held || held_desc.Width != desc.Width || held_desc.Height != desc.Height ||
            held_desc.Format != desc.Format) {
            D3D11_TEXTURE2D_DESC copy = desc;
            copy.Usage = D3D11_USAGE_DEFAULT;
            copy.BindFlags = 0;
            copy.CPUAccessFlags = 0;
            copy.MiscFlags = 0;
            session.held.Reset();
            if (FAILED(session.reader->device()->get()->CreateTexture2D(&copy, nullptr,
                                                                        &session.held))) {
                return;
            }
        }
        const d3d11::Lock lock(*session.reader->device());
        session.reader->device()->context()->CopyResource(session.held.Get(), &texture);
        session.held_at = captured;
    }

    const std::wstring device_name_;
    const std::string token_;
    const ScreenCaptureOptions options_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    std::jthread thread_;
};

}  // namespace

Result<std::unique_ptr<ScreenCapture>> duplicate_monitor(std::string_view token,
                                                         const ScreenCaptureOptions& options,
                                                         Clock& clock) {
    if (!token.starts_with(kMonitor)) return fail(Errc::InvalidArgument);
    std::wstring name = wide(token.substr(kMonitor.size()));
    std::optional<Output> output = find_output(name);
    if (!output) return fail(Errc::InvalidArgument);
    // Tried once: a system that never duplicates says so now, one that may
    // not yet (the secure desktop) is waited out by the capture.
    Result<std::shared_ptr<d3d11::Device>> device = d3d11::Device::create(output->adapter.Get());
    if (!device) return fail(device.error());
    ComPtr<IDXGIOutputDuplication> duplication;
    const HRESULT tried = duplicate(*output->output.Get(), *(*device)->get(), duplication);
    if (tried == DXGI_ERROR_UNSUPPORTED || tried == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ||
        tried == DXGI_ERROR_SESSION_DISCONNECTED) {
        LOG_INFO("engine", "no screen duplication here: 0x" << std::hex << unsigned(tried));
        return fail(Errc::Unavailable);
    }
    return std::unique_ptr<ScreenCapture>(
        std::make_unique<Duplication>(std::move(name), std::string(token), options, clock));
}

}  // namespace ratcast::dxgi
