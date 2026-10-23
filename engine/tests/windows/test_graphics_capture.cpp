// Windows.Graphics.Capture on this desktop, against a window the test draws
// itself on a thread of its own: its pictures come in its colour, at its size
// as it changes, no faster than asked; the capture ends when it closes; its
// restore token finds it again, unasked. And the primary screen, captured
// likewise. Skipped where this system cannot capture.

#include "windows/graphics_capture.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ratcast/engine/screen_capture.h"
#include "ratcast/engine/video_convert.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr auto kPatience = 5s;
/// A class of this process's own: test processes side by side each find
/// their own window by its token.
const std::wstring kClassName = L"RatcastCaptureTest" + std::to_wstring(GetCurrentProcessId());

/// A window filled with one colour — or, flickering, a new one every few
/// milliseconds — drawn on a thread of its own.
class TestWindow {
public:
    explicit TestWindow(COLORREF colour, bool flickering = false)
        : colour_(colour), flickering_(flickering) {
        std::promise<HWND> made;
        std::future<HWND> window = made.get_future();
        thread_ = std::thread([this, made = std::move(made)]() mutable { run(made); });
        window_ = window.get();
    }
    ~TestWindow() {
        close();
        thread_.join();
    }

    HWND get() const { return window_; }
    void close() { PostMessageW(window_, WM_CLOSE, 0, 0); }
    void resize(int width, int height) {
        SetWindowPos(window_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
    }

    /// The window's size on screen, its frame included: what is captured.
    SIZE size() const {
        RECT bounds{};
        GetWindowRect(window_, &bounds);
        return {bounds.right - bounds.left, bounds.bottom - bounds.top};
    }

private:
    void run(std::promise<HWND>& made) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &TestWindow::procedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName.c_str();
        RegisterClassW(&wc);  // fails harmlessly once registered
        const HWND window =
            CreateWindowExW(0, kClassName.c_str(), L"ratcast capture test", WS_OVERLAPPEDWINDOW,
                            100, 100, 400, 300, nullptr, nullptr, wc.hInstance, this);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        UpdateWindow(window);
        if (flickering_) SetTimer(window, 1, 5, nullptr);
        made.set_value(window);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_NCCREATE) {
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        }
        auto* self = reinterpret_cast<TestWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT paint;
                const HDC dc = BeginPaint(window, &paint);
                const HBRUSH brush = CreateSolidBrush(self->colour_);
                FillRect(dc, &paint.rcPaint, brush);
                DeleteObject(brush);
                EndPaint(window, &paint);
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_TIMER:
                self->colour_ = RGB(GetRValue(self->colour_) ^ 1, GetGValue(self->colour_),
                                    GetBValue(self->colour_));
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window, message, w, l);
    }

    COLORREF colour_;
    const bool flickering_;
    HWND window_ = nullptr;
    std::thread thread_;
};

/// What a capture handed out, for the test thread to wait on.
class Seen {
public:
    VideoSource::Sink sink() {
        return [this](VideoFrame picture) {
            std::lock_guard lock(mutex_);
            pictures_.push_back(std::move(picture));
            changed_.notify_all();
        };
    }
    VideoSource::EndHandler ended() {
        return [this](Result<void> outcome) {
            std::lock_guard lock(mutex_);
            outcome_ = outcome;
            changed_.notify_all();
        };
    }

    /// The first picture `good` says yes to, waiting kPatience at most.
    template <typename Good>
    std::optional<VideoFrame> wait_for(Good good) {
        std::unique_lock lock(mutex_);
        std::optional<VideoFrame> found;
        changed_.wait_for(lock, kPatience, [&] {
            for (const VideoFrame& picture : pictures_) {
                if (good(picture)) {
                    found = picture;
                    return true;
                }
            }
            return false;
        });
        return found;
    }
    std::optional<VideoFrame> wait_for_any() {
        return wait_for([](const VideoFrame&) { return true; });
    }

    std::optional<Result<void>> wait_for_end() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, kPatience, [&] { return outcome_.has_value(); });
        return outcome_;
    }

    size_t count() {
        std::lock_guard lock(mutex_);
        return pictures_.size();
    }

    /// "WxH" of the latest picture, for a failure to say.
    std::string last_size() {
        std::lock_guard lock(mutex_);
        if (pictures_.empty()) return "none";
        const VideoFrame& picture = pictures_.back();
        return std::to_string(picture.width()) + "x" + std::to_string(picture.height());
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<VideoFrame> pictures_;
    std::optional<Result<void>> outcome_;
};

/// The pixel at the middle of a BGRA picture: blue, green, red.
std::array<int, 3> middle(const VideoFrame& picture) {
    const VideoFrame::Plane& plane = picture.plane(0);
    const uint8_t* pixel =
        plane.data + plane.stride * (picture.height() / 2) + size_t{picture.width() / 2} * 4;
    return {pixel[0], pixel[1], pixel[2]};
}

class GraphicsCaptureTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!wgc::supported()) GTEST_SKIP() << "this system cannot capture the screen";
    }

    std::unique_ptr<ScreenCapture> capture(HWND window, ScreenCaptureOptions options = {}) {
        Result<std::unique_ptr<ScreenCapture>> capture = wgc::capture_window(window, options);
        EXPECT_TRUE(capture);
        return capture ? std::move(*capture) : nullptr;
    }
};

}  // namespace

TEST_F(GraphicsCaptureTest, AWindowComesInItsColourAndSize) {
    TestWindow window(RGB(200, 40, 90));
    auto source = capture(window.get(), {.format = PixelFormat::BGRA});
    ASSERT_TRUE(source);
    Seen seen;
    ASSERT_TRUE(source->start(seen.sink(), seen.ended()));
    const std::optional<VideoFrame> picture = seen.wait_for([](const VideoFrame& picture) {
        const std::array<int, 3> bgr = middle(picture);
        return std::abs(bgr[2] - 200) <= 2 && std::abs(bgr[1] - 40) <= 2 &&
               std::abs(bgr[0] - 90) <= 2;
    });
    ASSERT_TRUE(picture) << "never in the window's colour";
    EXPECT_EQ(picture->format(), PixelFormat::BGRA);
    // The frame drawn around it is the system's; the size is within it.
    const SIZE size = window.size();
    EXPECT_NEAR(int(picture->width()), size.cx, 16);
    EXPECT_NEAR(int(picture->height()), size.cy, 16);
    EXPECT_LE(picture->capture_time(), SteadyClock::instance().now());
}

TEST_F(GraphicsCaptureTest, GpuPicturesComeInTheWindowsColour) {
    TestWindow window(RGB(200, 40, 90));
    // The colour as the CPU would convert it: Y, then Cb and Cr.
    const uint8_t bgra[16] = {90, 40, 200, 255, 90, 40, 200, 255,
                              90, 40, 200, 255, 90, 40, 200, 255};
    const VideoFrame::Plane pixels{bgra, 8};
    const VideoFrame one(PixelFormat::BGRA, 2, 2, {&pixels, 1}, nullptr, Timestamp{});
    const Result<VideoFrame> expected = convert_frame(one, PixelFormat::NV12);
    ASSERT_TRUE(expected);
    const std::array<int, 3> yuv = {expected->plane(0).data[0], expected->plane(1).data[0],
                                    expected->plane(1).data[1]};

    auto source = capture(window.get(), {.gpu = true});
    ASSERT_TRUE(source);
    // Each picture read as it comes and let go: the GPU's go back to its pool.
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<bool> matched;  ///< and whether that picture was in GPU memory
    ASSERT_TRUE(source->start(
        [&](VideoFrame picture) {
            const Result<VideoFrame> read = download_frame(picture);
            if (!read) return;
            const VideoFrame::Plane& luma = read->plane(0);
            const VideoFrame::Plane& chroma = read->plane(1);
            const size_t x = read->width() / 4 * 2;
            const size_t y = read->height() / 4 * 2;
            const int at[3] = {luma.data[luma.stride * y + x],
                               chroma.data[chroma.stride * y / 2 + x],
                               chroma.data[chroma.stride * y / 2 + x + 1]};
            for (int i = 0; i < 3; ++i) {
                if (std::abs(at[i] - yuv[i]) > 3) return;
            }
            std::lock_guard lock(mutex);
            matched = picture.gpu() != nullptr;
            changed.notify_all();
        },
        {}));
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, kPatience, [&] { return matched.has_value(); }))
        << "never in the window's colour";
    if (!*matched) GTEST_SKIP() << "the GPU cannot convert here: the pictures came in memory";
}

TEST_F(GraphicsCaptureTest, FourTwoZeroComesInEvenSizes) {
    TestWindow window(RGB(30, 160, 30));
    window.resize(401, 301);
    auto source = capture(window.get());
    ASSERT_TRUE(source);
    Seen seen;
    ASSERT_TRUE(source->start(seen.sink(), seen.ended()));
    const std::optional<VideoFrame> picture = seen.wait_for_any();
    ASSERT_TRUE(picture);
    EXPECT_EQ(picture->format(), PixelFormat::NV12);
    EXPECT_EQ(picture->width() % 2, 0u);
    EXPECT_EQ(picture->height() % 2, 0u);
}

TEST_F(GraphicsCaptureTest, AResizedWindowComesAtItsNewSize) {
    TestWindow window(RGB(30, 30, 160));
    auto source = capture(window.get(), {.format = PixelFormat::BGRA});
    ASSERT_TRUE(source);
    Seen seen;
    ASSERT_TRUE(source->start(seen.sink(), seen.ended()));
    ASSERT_TRUE(seen.wait_for_any());

    window.resize(640, 480);
    const SIZE size = window.size();
    const std::optional<VideoFrame> resized = seen.wait_for([&](const VideoFrame& picture) {
        return std::abs(int(picture.width()) - size.cx) <= 16 &&
               std::abs(int(picture.height()) - size.cy) <= 16;
    });
    EXPECT_TRUE(resized) << "never at " << size.cx << "x" << size.cy << ", the latest at "
                         << seen.last_size() << " of " << seen.count();
}

TEST_F(GraphicsCaptureTest, AWindowThatClosesEndsTheCapture) {
    TestWindow window(RGB(90, 90, 90));
    auto source = capture(window.get());
    ASSERT_TRUE(source);
    Seen seen;
    ASSERT_TRUE(source->start(seen.sink(), seen.ended()));
    ASSERT_TRUE(seen.wait_for_any());
    window.close();
    const std::optional<Result<void>> outcome = seen.wait_for_end();
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(*outcome);
}

TEST_F(GraphicsCaptureTest, PicturesComeNoFasterThanAsked) {
    TestWindow window(RGB(100, 50, 50), true);  // a new picture every 5 ms
    auto source = capture(window.get(), {.max_frame_rate = 10});
    ASSERT_TRUE(source);
    Seen seen;
    ASSERT_TRUE(source->start(seen.sink(), seen.ended()));
    ASSERT_TRUE(seen.wait_for_any());
    const size_t before = seen.count();
    std::this_thread::sleep_for(1s);
    const size_t taken = seen.count() - before;
    EXPECT_LE(taken, 12u);
    EXPECT_GE(taken, 5u) << "the window changed all along";
}

TEST_F(GraphicsCaptureTest, StopsAndStartsAgain) {
    TestWindow window(RGB(10, 120, 200));
    auto source = capture(window.get());
    ASSERT_TRUE(source);
    for (int round = 0; round < 2; ++round) {
        Seen seen;
        ASSERT_TRUE(source->start(seen.sink(), seen.ended()));
        EXPECT_FALSE(source->start(seen.sink(), seen.ended()));  // already started
        EXPECT_TRUE(seen.wait_for_any()) << "round " << round;
        source->stop();
    }
}

TEST_F(GraphicsCaptureTest, ARestoreTokenFindsTheWindowUnasked) {
    TestWindow window(RGB(220, 220, 20));
    const std::string token = wgc::restore_token(window.get());
    ASSERT_FALSE(token.empty());
    EXPECT_EQ(wgc::find_window(token), window.get());
    EXPECT_EQ(wgc::find_monitor(token), nullptr) << "a window's token names no screen";

    Result<std::unique_ptr<ScreenCapture>> capture = open_screen_capture(
        {.monitors = false, .windows = true, .format = PixelFormat::BGRA, .restore_token = token});
    ASSERT_TRUE(capture);
    EXPECT_EQ((*capture)->restore_token(), token);
    Seen seen;
    ASSERT_TRUE((*capture)->start(seen.sink(), seen.ended()));
    EXPECT_TRUE(seen.wait_for([](const VideoFrame& picture) {
        const std::array<int, 3> bgr = middle(picture);
        return std::abs(bgr[2] - 220) <= 2 && std::abs(bgr[0] - 20) <= 2;
    })) << "another window than the test's";
}

TEST_F(GraphicsCaptureTest, TheScreenIsCapturedAndItsTokenFindsItAgain) {
    const HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const std::string token = wgc::restore_token(primary);
    ASSERT_FALSE(token.empty());
    EXPECT_EQ(wgc::find_monitor(token), primary);

    Result<std::unique_ptr<ScreenCapture>> capture =
        open_screen_capture({.monitors = true, .windows = false, .restore_token = token});
    ASSERT_TRUE(capture);
    Seen seen;
    ASSERT_TRUE((*capture)->start(seen.sink(), seen.ended()));
    const std::optional<VideoFrame> picture = seen.wait_for_any();
    ASSERT_TRUE(picture) << "a screen sends its first picture at once";
    EXPECT_GT(picture->width(), 0u);
    EXPECT_EQ(picture->width() % 2, 0u);
}

TEST(ScreenCaptureTest, CalledOffBeforeAskingIsCancelled) {
    std::stop_source stop;
    stop.request_stop();
    Result<std::unique_ptr<ScreenCapture>> capture = open_screen_capture({}, stop.get_token());
    ASSERT_FALSE(capture);
    EXPECT_EQ(capture.error(), Errc::Cancelled);
}

TEST(ScreenCaptureTest, OptionsThatOfferNothingAreRefused) {
    Result<std::unique_ptr<ScreenCapture>> capture =
        open_screen_capture({.monitors = false, .windows = false});
    ASSERT_FALSE(capture);
    EXPECT_EQ(capture.error(), Errc::InvalidArgument);
}
