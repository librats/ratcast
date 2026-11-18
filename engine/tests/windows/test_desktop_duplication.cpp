// DXGI Desktop Duplication against the machine's own primary screen: a window
// of the test's own, in a colour of its own, found where it is.

#include <gtest/gtest.h>

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ratcast/engine/screen_capture.h"
#include "windows/d3d11.h"

namespace ratcast {
namespace {

using namespace std::chrono_literals;

const std::wstring kClassName = L"RatcastDuplicationTest" + std::to_wstring(GetCurrentProcessId());

/// A popup window filled with one colour, topmost, on a thread of its own.
class Patch {
public:
    Patch(RECT where, COLORREF colour) : colour_(colour) {
        std::promise<HWND> made;
        std::future<HWND> window = made.get_future();
        thread_ =
            std::thread([this, where, made = std::move(made)]() mutable { run(where, made); });
        window_ = window.get();
    }
    ~Patch() {
        PostMessageW(window_, WM_CLOSE, 0, 0);
        thread_.join();
    }
    void set_colour(COLORREF colour) {
        colour_ = colour;
        InvalidateRect(window_, nullptr, FALSE);
    }

private:
    void run(RECT where, std::promise<HWND>& made) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &Patch::procedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName.c_str();
        RegisterClassW(&wc);
        const HWND window =
            CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName.c_str(),
                            L"", WS_POPUP, where.left, where.top, where.right - where.left,
                            where.bottom - where.top, nullptr, nullptr, wc.hInstance, this);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        UpdateWindow(window);
        made.set_value(window);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_NCCREATE) {
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        }
        auto* self = reinterpret_cast<Patch*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT paint;
                const HDC dc = BeginPaint(window, &paint);
                const HBRUSH brush = CreateSolidBrush(self->colour_.load());
                FillRect(dc, &paint.rcPaint, brush);
                DeleteObject(brush);
                EndPaint(window, &paint);
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window, message, w, l);
    }

    std::atomic<COLORREF> colour_;
    HWND window_ = nullptr;
    std::thread thread_;
};

struct Primary {
    std::string token;
    RECT bounds{};
};

Primary primary() {
    Primary found;
    MONITORINFOEXW info{};
    info.cbSize = sizeof info;
    GetMonitorInfoW(MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY), &info);
    found.bounds = info.rcMonitor;
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, info.szDevice, -1, nullptr, 0, nullptr, nullptr);
    std::string device(size_t(std::max(size - 1, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, info.szDevice, -1, device.data(), size, nullptr, nullptr);
    found.token = "monitor:" + device;
    return found;
}

/// The newest picture handed out, for the test thread to wait on.
class Newest {
public:
    VideoSource::Sink sink() {
        return [this](VideoFrame picture) {
            std::lock_guard lock(mutex_);
            ++count_;
            picture_ = std::move(picture);
            changed_.notify_all();
        };
    }
    template <typename Good>
    std::optional<VideoFrame> wait(Good good, std::chrono::milliseconds patience = 5s) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, patience, [&] { return picture_ && good(*picture_); }))
            return std::nullopt;
        return picture_;
    }
    int count() {
        std::lock_guard lock(mutex_);
        return count_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<VideoFrame> picture_;
    int count_ = 0;
};

/// The BGRA pixel at (x, y) of `picture`, as B, G, R.
std::array<uint8_t, 3> pixel(const VideoFrame& picture, uint32_t x, uint32_t y) {
    const uint8_t* at = picture.plane(0).data + size_t(y) * picture.plane(0).stride + size_t(x) * 4;
    return {at[0], at[1], at[2]};
}

bool close_to(std::array<uint8_t, 3> a, std::array<uint8_t, 3> b) {
    for (size_t i = 0; i < 3; ++i) {
        if (std::abs(int(a[i]) - int(b[i])) > 6) return false;
    }
    return true;
}

Result<std::unique_ptr<ScreenCapture>> open(const Primary& screen, PixelFormat format, bool gpu,
                                            uint32_t rate = 60) {
    ScreenCaptureOptions options;
    options.windows = false;
    options.format = format;
    options.gpu = gpu;
    options.max_frame_rate = rate;
    options.restore_token = screen.token;
    options.desktop_duplication = true;
    return open_screen_capture(options);
}

TEST(DesktopDuplication, ThePrimaryScreenComesWithAWindowWhereItIs) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const Primary screen = primary();
    auto capture = open(screen, PixelFormat::BGRA, false);
    if (!capture && capture.error() == Errc::Unavailable) GTEST_SKIP() << "no duplication here";
    ASSERT_TRUE(capture) << to_string(capture.error());
    EXPECT_EQ((*capture)->restore_token(), screen.token);

    const RECT where{screen.bounds.left + 50, screen.bounds.top + 60, screen.bounds.left + 250,
                     screen.bounds.top + 160};
    Patch patch(where, RGB(10, 200, 30));
    Newest newest;
    ASSERT_TRUE((*capture)->start(newest.sink(), {}));
    const uint32_t width = uint32_t(screen.bounds.right - screen.bounds.left);
    const auto seen = newest.wait([&](const VideoFrame& picture) {
        return picture.width() == width && close_to(pixel(picture, 150, 110), {30, 200, 10});
    });
    ASSERT_TRUE(seen) << "the window not seen at its place";
    EXPECT_EQ(seen->height(), uint32_t(screen.bounds.bottom - screen.bounds.top));

    // A last change is never lost, however few pictures a second are let through.
    (*capture)->stop();
    auto slow = open(screen, PixelFormat::BGRA, false, 2);
    ASSERT_TRUE(slow);
    Newest later;
    ASSERT_TRUE((*slow)->start(later.sink(), {}));
    ASSERT_TRUE(later.wait([](const VideoFrame&) { return true; }));
    for (int i = 0; i < 10; ++i) {
        patch.set_colour(RGB(200, uint8_t(i * 20), 40));
        std::this_thread::sleep_for(20ms);
    }
    patch.set_colour(RGB(40, 60, 220));
    EXPECT_TRUE(later.wait([&](const VideoFrame& picture) {
        return close_to(pixel(picture, 150, 110), {220, 60, 40});
    })) << "the last colour never came";
    (*slow)->stop();
}

TEST(DesktopDuplication, PicturesStayOnTheGpuAsNv12) {
    const Primary screen = primary();
    auto capture = open(screen, PixelFormat::NV12, true);
    if (!capture && capture.error() == Errc::Unavailable) GTEST_SKIP() << "no duplication here";
    ASSERT_TRUE(capture);
    Patch patch({screen.bounds.left + 10, screen.bounds.top + 10, screen.bounds.left + 90,
                 screen.bounds.top + 90},
                RGB(255, 0, 255));
    Newest newest;
    ASSERT_TRUE((*capture)->start(newest.sink(), {}));
    const auto seen = newest.wait([](const VideoFrame&) { return true; });
    (*capture)->stop();
    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->format(), PixelFormat::NV12);
    EXPECT_EQ(seen->width() % 2, 0u);
    if (!seen->gpu()) GTEST_SKIP() << "the GPU cannot convert here (WARP)";
    EXPECT_NE(d3d11::picture_of(*seen), nullptr);
}

TEST(DesktopDuplication, AScreenNotThereIsNotOpened) {
    ScreenCaptureOptions options;
    options.windows = false;
    options.desktop_duplication = true;
    options.restore_token = "monitor:\\\\.\\DISPLAY99";
    EXPECT_FALSE(open_screen_capture(options));
}

}  // namespace
}  // namespace ratcast
