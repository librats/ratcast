// Windows.Graphics.Capture through C++/WinRT. Its exceptions stop here: every
// entry point catches winrt::hresult_error and answers with an Errc.
//
// The frame pool is free-threaded: it hands frames out on the thread pool,
// one at a time. Everything a frame needs lives in a Stream the handlers hold
// only weakly, under its mutex, so that stop() — which closes the stream under
// that mutex — waits out a frame being handled and none comes after it.

#include "windows/graphics_capture.h"

#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <shobjidl_core.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "capture/frame_pacer.h"
#include "ratcast/engine/video_convert.h"
#include "windows/com.h"
#include "windows/qpc.h"

#include "librats/util/logger.h"

namespace ratcast::wgc {
namespace {

namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;
using win::ComPtr;

constexpr std::string_view kMonitor = "monitor:";
constexpr std::string_view kWindow = "window:";
constexpr auto kPixels = directx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
/// Frames the pool holds: one being read while the next is drawn.
constexpr int32_t kFrames = 2;
/// How often a window captured is looked at, to tell when it closes.
constexpr DWORD kWatchPeriodMs = 200;

std::string utf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::wstring window_text(HWND window) {
    std::wstring text(size_t(GetWindowTextLengthW(window)) + 1, L'\0');
    text.resize(size_t(GetWindowTextW(window, text.data(), int(text.size()))));
    return text;
}

/// A window the picker would offer: a top-level one, shown and not cloaked
/// (another virtual desktop's, a suspended app's).
bool shown(HWND window) {
    if (!IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    return cloaked == 0;
}

/// `visit(window)` for each window shown, topmost first, until it says true.
template <typename Visit>
HWND find_shown(Visit visit) {
    struct Search {
        Visit& visit;
        HWND found = nullptr;
    } search{visit};
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(param);
            if (!shown(window) || !search.visit(window)) return TRUE;
            search.found = window;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

template <typename Visit>
HMONITOR find_monitor_where(Visit visit) {
    struct Search {
        Visit& visit;
        HMONITOR found = nullptr;
    } search{visit};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(param);
            if (!search.visit(monitor)) return TRUE;
            search.found = monitor;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

capture::GraphicsCaptureItem item_for(HMONITOR monitor) {
    const auto interop =
        winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    capture::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForMonitor(
        monitor, winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

capture::GraphicsCaptureItem item_for(HWND window) {
    const auto interop =
        winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    capture::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForWindow(
        window, winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

/// What the picker handed out, which says only its name and size: the screen
/// whose own item says the same, or else the topmost window of that title.
struct Chosen {
    std::string token;
    HWND window = nullptr;  ///< for a window
};

Chosen chosen(const capture::GraphicsCaptureItem& item) {
    const std::wstring name(item.DisplayName());
    const winrt::Windows::Graphics::SizeInt32 size = item.Size();
    const HMONITOR monitor = find_monitor_where([&](HMONITOR candidate) {
        try {
            const capture::GraphicsCaptureItem other = item_for(candidate);
            const winrt::Windows::Graphics::SizeInt32 other_size = other.Size();
            return other.DisplayName() == name && other_size.Width == size.Width &&
                   other_size.Height == size.Height;
        } catch (const winrt::hresult_error&) {
            return false;
        }
    });
    if (monitor) return {restore_token(monitor)};
    const HWND window = find_shown([&](HWND candidate) { return window_text(candidate) == name; });
    if (!window) return {};
    return {restore_token(window), window};
}

/// A device to receive frames on, and the WinRT face of it the pool takes.
struct Device {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt{nullptr};
};

Device make_device() {
    Device device;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr,
                                       0, D3D11_SDK_VERSION, &device.d3d, nullptr, &device.context);
    if (FAILED(result)) {  // no GPU of its own (a virtual machine): the software one
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, &device.d3d, nullptr, &device.context);
    }
    winrt::check_hresult(result);
    // The capture's threads and the pool's use it both.
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(device.d3d.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice> dxgi;
    winrt::check_hresult(device.d3d.As(&dxgi));
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
    device.winrt = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    return device;
}

Errc errc_of(const winrt::hresult_error& error) {
    LOG_WARN("engine", "screen capture failed: " << utf8(error.message()));
    return win::errc_of(error.code());
}

/// One capture, from start() to stop().
class Stream final : public std::enable_shared_from_this<Stream> {
public:
    using Sink = VideoSource::Sink;
    using EndHandler = VideoSource::EndHandler;

    Stream(capture::GraphicsCaptureItem item, HWND window, const ScreenCaptureOptions& options,
           Clock& clock, Sink sink, EndHandler ended)
        : item_(std::move(item)),
          window_(window),
          options_(options),
          clock_(clock),
          sink_(std::move(sink)),
          ended_(std::move(ended)),
          pacer_(options.max_frame_rate) {}

    Result<void> open() {
        std::lock_guard lock(mutex_);
        if (window_) {
            // The item says when another process's window closes, but not
            // this one's: the window is looked at a few times a second.
            watch_ = CreateThreadpoolTimer(&Stream::on_watch, this, nullptr);
            if (!watch_) return fail(Errc::Internal);
            FILETIME due{};  // at once, then every period
            SetThreadpoolTimer(watch_, &due, kWatchPeriodMs, 0);
        }
        try {
            device_ = make_device();
            size_ = item_.Size();
            pool_ = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(device_.winrt, kPixels,
                                                                            kFrames, size_);
            session_ = pool_.CreateCaptureSession(item_);
            configure();
            const std::weak_ptr<Stream> self = weak_from_this();
            arrived_ = pool_.FrameArrived(winrt::auto_revoke, [self](auto&&, auto&&) {
                if (const std::shared_ptr<Stream> stream = self.lock()) stream->on_frame();
            });
            closed_ = item_.Closed(winrt::auto_revoke, [self](auto&&, auto&&) {
                if (const std::shared_ptr<Stream> stream = self.lock()) stream->end(Result<void>{});
            });
            running_ = true;
            session_.StartCapture();
            return {};
        } catch (const winrt::hresult_error& error) {
            running_ = false;
            return fail(errc_of(error));
        }
    }

    void close() {
        if (watch_) {  // before the lock, which a look at the window may be waiting for
            SetThreadpoolTimer(watch_, nullptr, 0, 0);
            WaitForThreadpoolTimerCallbacks(watch_, TRUE);
            CloseThreadpoolTimer(watch_);
            watch_ = nullptr;
        }
        std::lock_guard lock(mutex_);
        running_ = false;
        arrived_.revoke();
        closed_.revoke();
        try {
            if (session_) session_.Close();
            if (pool_) pool_.Close();
        } catch (const winrt::hresult_error&) {}
        session_ = nullptr;
        pool_ = nullptr;
    }

private:
    /// The pointer, the yellow border, how often: as far as this Windows goes.
    void configure() {
        try {
            session_.IsCursorCaptureEnabled(options_.cursor);  // Windows 10 2004
        } catch (const winrt::hresult_error&) {}
        try {
            session_.IsBorderRequired(false);  // Windows 11
        } catch (const winrt::hresult_error&) {}
        if (options_.max_frame_rate > 0) {
            try {  // Windows 11 24H2: no more pictures drawn than are taken
                session_.MinUpdateInterval(
                    std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                        std::chrono::microseconds(1'000'000 / options_.max_frame_rate)));
            } catch (const winrt::hresult_error&) {}
        }
    }

    void on_frame() {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        std::optional<VideoFrame> picture;
        try {
            const capture::Direct3D11CaptureFrame frame = pool_.TryGetNextFrame();
            if (!frame) return;
            const winrt::Windows::Graphics::SizeInt32 content = frame.ContentSize();
            if (content.Width > size_.Width || content.Height > size_.Height) {
                // Grown past the frames the pool makes, which cut this one
                // short: larger ones from now on, and the window asked to
                // draw itself again into them. Smaller content fits as it is.
                size_ = {std::max(content.Width, size_.Width),
                         std::max(content.Height, size_.Height)};
                pool_.Recreate(device_.winrt, kPixels, kFrames, size_);
                if (window_) RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
            }
            if (!pacer_.admit(clock_.now())) return;
            // Stamped with when it is to be shown, which may be the coming
            // refresh: never later than now.
            const Timestamp captured =
                std::min(win::at_qpc(frame.SystemRelativeTime().count(), clock_), clock_.now());
            const auto access =
                frame.Surface()
                    .as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
            picture = read(*texture.Get(), content, captured);
        } catch (const winrt::hresult_error& error) {
            // The GPU went (a driver update, the device removed).
            running_ = false;
            if (ended_) ended_(fail(errc_of(error)));
            return;
        }
        if (picture) sink_(std::move(*picture));
    }

    /// The picture in `texture`, as much of it as `content` covers, in the
    /// format asked for.
    std::optional<VideoFrame> read(ID3D11Texture2D& texture,
                                   winrt::Windows::Graphics::SizeInt32 content,
                                   Timestamp captured) {
        D3D11_TEXTURE2D_DESC desc;
        texture.GetDesc(&desc);
        uint32_t width = std::min(uint32_t(std::max(content.Width, 0)), desc.Width);
        uint32_t height = std::min(uint32_t(std::max(content.Height, 0)), desc.Height);
        if (options_.format != PixelFormat::BGRA) {  // 4:2:0 wants even sizes
            width &= ~1u;
            height &= ~1u;
        }
        if (width == 0 || height == 0) return std::nullopt;

        if (!staging_ || staging_width_ != width || staging_height_ != height) {
            D3D11_TEXTURE2D_DESC staging{};
            staging.Width = width;
            staging.Height = height;
            staging.MipLevels = 1;
            staging.ArraySize = 1;
            staging.Format = desc.Format;
            staging.SampleDesc.Count = 1;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_.Reset();
            winrt::check_hresult(device_.d3d->CreateTexture2D(&staging, nullptr, &staging_));
            staging_width_ = width;
            staging_height_ = height;
        }
        const D3D11_BOX box{0, 0, 0, width, height, 1};
        device_.context->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, &texture, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(device_.context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const VideoFrame::Plane plane{static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch};
        const VideoFrame view(PixelFormat::BGRA, width, height, {&plane, 1}, nullptr, captured);
        Result<VideoFrame> picture = convert_frame(view, options_.format, options_.color_space);
        device_.context->Unmap(staging_.Get(), 0);
        if (!picture) return std::nullopt;
        return std::move(*picture);
    }

    void end(Result<void> outcome) {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        running_ = false;
        if (ended_) ended_(outcome);
    }

    static void CALLBACK on_watch(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) {
        auto& stream = *static_cast<Stream*>(context);
        if (!IsWindow(stream.window_)) stream.end(Result<void>{});
    }

    const capture::GraphicsCaptureItem item_;
    const HWND window_;  ///< the window captured; none for a screen
    const ScreenCaptureOptions options_;
    Clock& clock_;
    const Sink sink_;
    const EndHandler ended_;

    std::mutex mutex_;  ///< everything below
    bool running_ = false;
    FramePacer pacer_;
    Device device_;
    winrt::Windows::Graphics::SizeInt32 size_{};  ///< of the frames the pool makes
    capture::Direct3D11CaptureFramePool pool_{nullptr};
    capture::GraphicsCaptureSession session_{nullptr};
    capture::Direct3D11CaptureFramePool::FrameArrived_revoker arrived_;
    capture::GraphicsCaptureItem::Closed_revoker closed_;
    ComPtr<ID3D11Texture2D> staging_;  ///< what the CPU reads a frame from
    uint32_t staging_width_ = 0;
    uint32_t staging_height_ = 0;

    PTP_TIMER watch_ = nullptr;  ///< looking at the window, if one is captured
};

class GraphicsCapture final : public ScreenCapture {
public:
    GraphicsCapture(capture::GraphicsCaptureItem item, HWND window,
                    const ScreenCaptureOptions& options, std::string token, Clock& clock)
        : item_(std::move(item)),
          window_(window),
          options_(options),
          token_(std::move(token)),
          clock_(clock) {}
    ~GraphicsCapture() override { stop(); }

    Result<void> start(Sink sink, EndHandler ended) override {
        std::lock_guard lock(mutex_);
        if (stream_) return fail(Errc::InvalidArgument);
        win::keep_multithreaded_apartment();
        auto stream = std::make_shared<Stream>(item_, window_, options_, clock_, std::move(sink),
                                               std::move(ended));
        if (Result<void> opened = stream->open(); !opened) {
            stream->close();
            return opened;
        }
        stream_ = std::move(stream);
        return {};
    }

    void stop() override {
        std::lock_guard lock(mutex_);
        if (!stream_) return;
        stream_->close();
        stream_.reset();
    }

    const std::string& restore_token() const noexcept override { return token_; }

private:
    const capture::GraphicsCaptureItem item_;
    const HWND window_;
    const ScreenCaptureOptions options_;
    const std::string token_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    std::shared_ptr<Stream> stream_;
};

Result<std::unique_ptr<ScreenCapture>> pick_here(const ScreenCaptureOptions& options,
                                                 const std::stop_token& stop, Clock& clock) {
    win::keep_multithreaded_apartment();
    try {
        capture::GraphicsCapturePicker picker;
        // Shown over whatever window asked for it: the app's, or the console's.
        HWND owner = GetForegroundWindow();
        if (!owner) owner = GetConsoleWindow();
        winrt::check_hresult(picker.as<IInitializeWithWindow>()->Initialize(owner));
        const auto choosing = picker.PickSingleItemAsync();
        const std::stop_callback cancel(stop, [&choosing] { choosing.Cancel(); });
        capture::GraphicsCaptureItem item = choosing.get();
        if (!item || stop.stop_requested()) return fail(Errc::Cancelled);
        Chosen what = chosen(item);
        LOG_INFO("engine", "capturing " << utf8(item.DisplayName()));
        return std::unique_ptr<ScreenCapture>(std::make_unique<GraphicsCapture>(
            std::move(item), what.window, options, std::move(what.token), clock));
    } catch (const winrt::hresult_canceled&) {
        return fail(Errc::Cancelled);
    } catch (const winrt::hresult_error& error) {
        return fail(errc_of(error));
    }
}

}  // namespace

bool supported() {
    win::keep_multithreaded_apartment();
    try {
        return capture::GraphicsCaptureSession::IsSupported();
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

std::string restore_token(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof info;
    if (!GetMonitorInfoW(monitor, &info)) return {};
    return std::string(kMonitor) + utf8(info.szDevice);
}

std::string restore_token(HWND window) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    const win::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
    if (!process) return {};
    wchar_t path[MAX_PATH * 4];
    DWORD length = DWORD(std::size(path));
    if (!QueryFullProcessImageNameW(process.get(), 0, path, &length)) return {};
    wchar_t name[256];
    const int name_length = GetClassNameW(window, name, int(std::size(name)));
    return std::string(kWindow) + utf8({path, length}) + "|" +
           utf8({name, size_t(std::max(name_length, 0))});
}

HMONITOR find_monitor(std::string_view token) {
    if (!token.starts_with(kMonitor)) return nullptr;
    return find_monitor_where(
        [token](HMONITOR monitor) { return restore_token(monitor) == token; });
}

HWND find_window(std::string_view token) {
    if (!token.starts_with(kWindow)) return nullptr;
    return find_shown([token](HWND window) { return restore_token(window) == token; });
}

Result<std::unique_ptr<ScreenCapture>> capture_monitor(HMONITOR monitor,
                                                       const ScreenCaptureOptions& options,
                                                       Clock& clock) {
    win::keep_multithreaded_apartment();
    try {
        return std::unique_ptr<ScreenCapture>(std::make_unique<GraphicsCapture>(
            item_for(monitor), nullptr, options, restore_token(monitor), clock));
    } catch (const winrt::hresult_error& error) {
        return fail(errc_of(error));
    }
}

Result<std::unique_ptr<ScreenCapture>> capture_window(HWND window,
                                                      const ScreenCaptureOptions& options,
                                                      Clock& clock) {
    win::keep_multithreaded_apartment();
    try {
        return std::unique_ptr<ScreenCapture>(std::make_unique<GraphicsCapture>(
            item_for(window), window, options, restore_token(window), clock));
    } catch (const winrt::hresult_error& error) {
        return fail(errc_of(error));
    }
}

Result<std::unique_ptr<ScreenCapture>> pick(const ScreenCaptureOptions& options,
                                            std::stop_token stop, Clock& clock) {
    if (stop.stop_requested()) return fail(Errc::Cancelled);
    // Waited for on a thread of its own, in COM's multithreaded apartment,
    // whatever the thread that asks is in (a UI's is single-threaded).
    Result<std::unique_ptr<ScreenCapture>> picked = fail(Errc::Internal);
    std::thread([&] { picked = pick_here(options, stop, clock); }).join();
    return picked;
}

}  // namespace ratcast::wgc
