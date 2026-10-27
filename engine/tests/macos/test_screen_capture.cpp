// Screen capture on macOS, as a test process has it: with no application to
// show the picker in, the main screen — in each format asked, of even sizes,
// no faster than asked, NV12 left on its IOSurfaces when GPU pictures are
// asked — named by a restore token that finds it again; and
// the options that offer nothing, or a stop already asked, refused. Skipped
// without the leave to record the screen (CI's runner).

#include "ratcast/engine/screen_capture.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

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

    /// Blocks until `count` pictures have come, or 3 s have passed.
    std::vector<VideoFrame> wait_for(size_t count) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, 3s, [&] { return pictures_.size() >= count; });
        return pictures_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<VideoFrame> pictures_;
};

/// The main screen, or skipped without the leave to record it.
std::unique_ptr<ScreenCapture> main_screen(ScreenCaptureOptions options) {
    options.windows = false;
    Result<std::unique_ptr<ScreenCapture>> capture = open_screen_capture(options);
    if (!capture) return nullptr;
    return std::move(*capture);
}

}  // namespace

TEST(MacScreenCaptureTest, OptionsThatOfferNothingAreRefused) {
    const Result<std::unique_ptr<ScreenCapture>> capture =
        open_screen_capture({.monitors = false, .windows = false});
    ASSERT_FALSE(capture);
    EXPECT_EQ(capture.error(), Errc::InvalidArgument);
}

TEST(MacScreenCaptureTest, AStopAlreadyAskedCancels) {
    std::stop_source stop;
    stop.request_stop();
    const Result<std::unique_ptr<ScreenCapture>> capture =
        open_screen_capture({}, stop.get_token());
    ASSERT_FALSE(capture);
    EXPECT_EQ(capture.error(), Errc::Cancelled);
}

TEST(MacScreenCaptureTest, TheMainScreenInEachFormat) {
    for (const PixelFormat format : {PixelFormat::NV12, PixelFormat::I420, PixelFormat::BGRA}) {
        std::unique_ptr<ScreenCapture> capture = main_screen({.format = format});
        if (!capture) GTEST_SKIP() << "no leave to record the screen";
        Seen seen;
        ASSERT_TRUE(capture->start(seen.sink(), {}));
        EXPECT_FALSE(capture->start(seen.sink(), {})) << "already started";
        // A still screen sends its first picture all the same.
        const std::vector<VideoFrame> pictures = seen.wait_for(1);
        capture->stop();
        ASSERT_FALSE(pictures.empty()) << int(format);
        const VideoFrame& picture = pictures.front();
        EXPECT_EQ(picture.format(), format);
        EXPECT_GT(picture.width(), 0u);
        EXPECT_GT(picture.height(), 0u);
        if (format != PixelFormat::BGRA) {
            EXPECT_EQ(picture.width() % 2, 0u);
            EXPECT_EQ(picture.height() % 2, 0u);
        }
        EXPECT_LE(picture.capture_time(), SteadyClock::instance().now());
        EXPECT_GE(picture.capture_time(), SteadyClock::instance().now() - 5s);
    }
}

TEST(MacScreenCaptureTest, GpuPicturesStayOnTheirSurfaces) {
    for (const PixelFormat format : {PixelFormat::NV12, PixelFormat::I420}) {
        std::unique_ptr<ScreenCapture> capture = main_screen({.format = format, .gpu = true});
        if (!capture) GTEST_SKIP() << "no leave to record the screen";
        Seen seen;
        ASSERT_TRUE(capture->start(seen.sink(), {}));
        const std::vector<VideoFrame> pictures = seen.wait_for(1);
        capture->stop();
        ASSERT_FALSE(pictures.empty()) << int(format);
        const VideoFrame& picture = pictures.front();
        EXPECT_EQ(picture.format(), format);
        if (format != PixelFormat::NV12) {  // only NV12 stays where it is
            EXPECT_FALSE(picture.gpu());
            continue;
        }
        ASSERT_TRUE(picture.gpu());
        EXPECT_EQ(picture.gpu()->device()->api(), GpuApi::CoreVideo);
        const Result<VideoFrame> read = download_frame(picture);
        ASSERT_TRUE(read);
        EXPECT_EQ(read->width(), picture.width());
        EXPECT_EQ(read->height(), picture.height());
        EXPECT_NE(read->plane(1).data, nullptr);
    }
}

TEST(MacScreenCaptureTest, ARestoreTokenFindsTheScreenAgain) {
    std::unique_ptr<ScreenCapture> first = main_screen({});
    if (!first) GTEST_SKIP() << "no leave to record the screen";
    const std::string token = first->restore_token();
    ASSERT_EQ(token.rfind("display:", 0), 0u) << token;
    first.reset();

    Result<std::unique_ptr<ScreenCapture>> again =
        open_screen_capture({.windows = false, .restore_token = token});
    ASSERT_TRUE(again);
    EXPECT_EQ((*again)->restore_token(), token);
    Seen seen;
    ASSERT_TRUE((*again)->start(seen.sink(), {}));
    EXPECT_FALSE(seen.wait_for(1).empty());
}

TEST(MacScreenCaptureTest, StopsAndStartsAgain) {
    std::unique_ptr<ScreenCapture> capture = main_screen({.max_frame_rate = 10});
    if (!capture) GTEST_SKIP() << "no leave to record the screen";
    for (int round = 0; round < 2; ++round) {
        Seen seen;
        ASSERT_TRUE(capture->start(seen.sink(), {}));
        EXPECT_FALSE(seen.wait_for(1).empty()) << "round " << round;
        capture->stop();
    }
}
