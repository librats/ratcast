// The screen on Android: MediaProjection's (android/projection.h), the whole
// screen — or, where Android 14 offers it, the one application the user
// chose in the system's dialog, whatever the options say. The screen is
// mirrored into a VirtualDisplay drawing into an AImageReader of RGBA
// pictures, no larger than kLongestSide on the long side, its proportions
// kept; each picture, as it comes on the reader's thread, converted into the
// format asked for. A turn of the screen is looked for twice a second, and
// the display mirrored into a reader of the turned size from then on. A
// reader is deleted only once no callback of its own is at work: deleting
// one holds its lock until its callbacks' thread is over, and a callback
// that reads a picture or lets one go waits for that lock. A
// projection stopped — by the user, from the status bar or the notification —
// ends the capture with success. There is no pointer to draw on a phone.

#include "ratcast/engine/screen_capture.h"

#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "android/projection.h"
#include "capture/frame_pacer.h"
#include "ratcast/engine/video_convert.h"

#include "librats/util/logger.h"

namespace ratcast {
namespace {

/// The longest side pictures have: a phone's screen is some 2400 pixels
/// long, which costs more to convert and encode than it shows a viewer.
constexpr uint32_t kLongestSide = 1920;
/// How often the screen's turn and the projection are looked at.
constexpr Duration kWatchEvery = std::chrono::milliseconds(500);
/// Pictures a reader holds: the one read, the one coming, one spare.
constexpr int32_t kHeldImages = 3;

struct ReaderDeleter {
    void operator()(AImageReader* reader) const { AImageReader_delete(reader); }
};
using Reader = std::unique_ptr<AImageReader, ReaderDeleter>;

/// A picture's size for `screen`: within kLongestSide, its proportions kept,
/// even.
std::pair<uint32_t, uint32_t> size_for(const android::Projection::Screen& screen) {
    const uint32_t longest = std::max(screen.width, screen.height);
    const double scale = longest > kLongestSide ? double(kLongestSide) / longest : 1.0;
    const auto even = [&](uint32_t side) { return std::max(2u, uint32_t(side * scale) & ~1u); };
    return {even(screen.width), even(screen.height)};
}

class AndroidScreenCapture final : public ScreenCapture {
public:
    AndroidScreenCapture(const ScreenCaptureOptions& options,
                         std::shared_ptr<android::Projection> projection,
                         Clock& clock = SteadyClock::instance())
        : options_(options),
          projection_(std::move(projection)),
          clock_(clock),
          pacer_(options.max_frame_rate) {}

    ~AndroidScreenCapture() override { stop(); }

    const std::string& restore_token() const noexcept override { return no_token_; }

    Result<void> start(Sink sink, EndHandler ended) override {
        std::lock_guard lock(start_mutex_);
        if (watcher_.joinable()) return fail(Errc::InvalidArgument);
        const std::optional<android::Projection::Screen> screen = projection_->screen();
        if (!screen) return fail(Errc::Unavailable);
        {
            std::lock_guard picture_lock(mutex_);
            sink_ = std::move(sink);
            ended_ = std::move(ended);
            over_ = false;
        }
        if (Result<void> shown = show(*screen); !shown) return shown;
        watcher_ = std::jthread([this](std::stop_token stop) { watch(stop); });
        return {};
    }

    void stop() override {
        std::lock_guard lock(start_mutex_);
        if (watcher_.joinable()) {
            watcher_.request_stop();
            watcher_.join();
        }
        {
            std::lock_guard picture_lock(mutex_);
            over_ = true;
        }
        if (reader_) {
            projection_->hide();
            retire(std::move(reader_));
        }
    }

private:
    /// The screen mirrored into a reader of its size.
    Result<void> show(const android::Projection::Screen& screen) {
        const auto [width, height] = size_for(screen);
        AImageReader* made = nullptr;
        if (AImageReader_new(int32_t(width), int32_t(height), AIMAGE_FORMAT_RGBA_8888, kHeldImages,
                             &made) != AMEDIA_OK) {
            return fail(Errc::Unavailable);
        }
        Reader reader(made);
        AImageReader_setImageListener(reader.get(), &listener_);
        ANativeWindow* window = nullptr;
        if (AImageReader_getWindow(reader.get(), &window) != AMEDIA_OK ||
            !projection_->show(window, width, height, screen.dpi)) {
            return fail(Errc::Unavailable);
        }
        LOG_INFO("engine", "the screen, " << screen.width << "x" << screen.height << ", as "
                                          << width << "x" << height);
        // The reader before goes once the display draws into this one.
        {
            std::lock_guard lock(mutex_);
            current_ = reader.get();
        }
        std::swap(reader_, reader);
        if (reader) retire(std::move(reader));
        shown_ = screen;
        return {};
    }

    /// `reader`, no longer current, deleted once no callback of its is at
    /// work; none starts any more.
    void retire(Reader reader) {
        AImageReader_setImageListener(reader.get(), nullptr);
        {
            std::unique_lock lock(mutex_);
            if (current_ == reader.get()) current_ = nullptr;
            idle_.wait(lock, [&] { return !busy_.contains(reader.get()); });
        }
        reader.reset();
    }

    /// The watcher's thread: the projection stopped, or the screen turned.
    void watch(std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any woken;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                if (woken.wait_for(lock, stop, kWatchEvery, [] { return false; }) ||
                    stop.stop_requested())
                    return;
            }
            if (projection_->stopped()) {
                LOG_INFO("engine", "the screen is shared no more: the projection stopped");
                end({});
                return;
            }
            const std::optional<android::Projection::Screen> screen = projection_->screen();
            if (!screen || (screen->width == shown_.width && screen->height == shown_.height))
                continue;
            if (Result<void> shown = show(*screen); !shown) {
                LOG_WARN("engine", "the turned screen is not shown");
                end(fail(shown.error()));
                return;
            }
        }
    }

    /// On a reader's thread: the newest picture, converted — unless the
    /// reader is no longer current (retire()).
    void take(AImageReader* reader) {
        {
            std::lock_guard lock(mutex_);
            if (over_ || reader != current_) return;
            ++busy_[reader];
        }
        convert(reader);
        std::lock_guard lock(mutex_);
        if (--busy_[reader] == 0) busy_.erase(reader);
        idle_.notify_all();
    }

    void convert(AImageReader* reader) {
        AImage* acquired = nullptr;
        if (AImageReader_acquireLatestImage(reader, &acquired) != AMEDIA_OK || !acquired) return;
        const std::unique_ptr<AImage, decltype(&AImage_delete)> image(acquired, AImage_delete);
        const Timestamp now = clock_.now();
        {
            std::lock_guard lock(mutex_);
            if (over_ || !pacer_.admit(now)) return;
        }

        AImageCropRect crop{};
        uint8_t* data = nullptr;
        int length = 0;
        int32_t row_step = 0;
        int32_t pixel_step = 0;
        if (AImage_getCropRect(image.get(), &crop) != AMEDIA_OK ||
            AImage_getPlaneData(image.get(), 0, &data, &length) != AMEDIA_OK ||
            AImage_getPlaneRowStride(image.get(), 0, &row_step) != AMEDIA_OK ||
            AImage_getPlanePixelStride(image.get(), 0, &pixel_step) != AMEDIA_OK || !data ||
            pixel_step != 4) {
            return;
        }
        // 4:2:0 wants whole pairs: an odd last row or column is left out.
        const uint32_t width = uint32_t(std::max(crop.right - crop.left, 0)) & ~1u;
        const uint32_t height = uint32_t(std::max(crop.bottom - crop.top, 0)) & ~1u;
        const int64_t first = int64_t(crop.top) * row_step + int64_t(crop.left) * 4;
        if (width == 0 || height == 0 || first < 0 ||
            first + int64_t(height - 1) * row_step + int64_t(width) * 4 > length) {
            return;
        }
        const VideoFrame::Plane plane{data + first, size_t(row_step)};
        const VideoFrame view(PixelFormat::RGBA, width, height, {&plane, 1}, nullptr, now);
        Result<VideoFrame> picture = convert_frame(view, options_.format, options_.color_space);
        std::lock_guard lock(mutex_);
        if (picture && !over_) sink_(std::move(*picture));
    }

    /// The end, said once.
    void end(Result<void> outcome) {
        std::lock_guard lock(mutex_);
        if (over_) return;
        over_ = true;
        if (ended_) ended_(std::move(outcome));
    }

    static void on_image(void* self, AImageReader* reader) {
        static_cast<AndroidScreenCapture*>(self)->take(reader);
    }

    const ScreenCaptureOptions options_;
    const std::shared_ptr<android::Projection> projection_;
    Clock& clock_;
    const std::string no_token_;  ///< Android keeps no consent
    AImageReader_ImageListener listener_{this, &AndroidScreenCapture::on_image};

    std::mutex start_mutex_;  ///< serialises start() and stop()
    Reader reader_;           ///< start()'s, then the watcher's
    android::Projection::Screen shown_;
    std::jthread watcher_;

    std::mutex mutex_;  ///< over the sink, the end, the pacer and the readers' callbacks
    std::condition_variable idle_;
    AImageReader* current_ = nullptr;    ///< the reader whose pictures are taken
    std::map<AImageReader*, int> busy_;  ///< callbacks at work, by their reader
    VideoSource::Sink sink_;
    VideoSource::EndHandler ended_;
    FramePacer pacer_;
    bool over_ = true;
};

}  // namespace

Result<std::unique_ptr<ScreenCapture>> open_screen_capture(const ScreenCaptureOptions& options,
                                                           std::stop_token stop) {
    if (!options.monitors && !options.windows) return fail(Errc::InvalidArgument);
    Result<std::shared_ptr<android::Projection>> projection = android::Projection::ask(stop);
    if (!projection) return fail(projection.error());
    return std::unique_ptr<ScreenCapture>(
        std::make_unique<AndroidScreenCapture>(options, std::move(*projection)));
}

}  // namespace ratcast
