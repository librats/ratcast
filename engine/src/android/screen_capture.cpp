// The screen on Android: MediaProjection's (android/projection.h), the whole
// screen — or, where Android 14 offers it, the one application the user
// chose in the system's dialog, whatever the options say. The screen is
// mirrored into a VirtualDisplay drawing into an AImageReader of RGBA
// pictures, no larger than kLongestSide on the long side, its proportions
// kept; each picture, as it comes on the reader's thread, converted into the
// format asked for — or, GPU pictures asked for, handed out as it is, in its
// AHardwareBuffer (android/hardware_picture.h), for MediaCodec's encoder to
// draw into its surface, the reader kept while any of its pictures is. A turn
// of the screen is looked for twice a second, and the display mirrored into
// a reader of the turned size from then on.
//
// A reader's callbacks reach the capture through a link of the reader's own
// (ReaderLink), which the capture is cut from, waiting for a callback at
// work, before the reader is let go of: a callback already on its way may
// come after that, as late as the reader's deletion, which ends its thread.
// So no callback reaches a capture that is gone, and deleting a reader —
// which holds its lock until its callbacks' thread is over — never waits for
// a callback that waits for that lock or the capture's. A projection stopped
// — by the user, from the status bar or the notification — ends the capture
// with success. There is no pointer to draw on a phone.

#include "ratcast/engine/screen_capture.h"

#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "android/hardware_picture.h"
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
/// Pictures a reader of GPU pictures holds, those the sink keeps among them:
/// the one encoded, the one waiting, the one kept to repeat.
constexpr int32_t kHeldGpuImages = 6;
/// What GPU pictures are drawn for: the GPU reads them, and so may the CPU,
/// for an encoder that does not draw them.
constexpr uint64_t kGpuUsage =
    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;

using Image = std::unique_ptr<AImage, decltype(&AImage_delete)>;

class AndroidScreenCapture;

/// A reader and what its callbacks reach the capture through, deleted
/// together once neither the capture nor a picture of the reader's holds
/// them.
struct ReaderLink : std::enable_shared_from_this<ReaderLink> {
    ~ReaderLink() {
        if (reader) AImageReader_delete(reader);
    }

    AImageReader* reader = nullptr;
    std::mutex mutex;  ///< held by a callback at work
    AndroidScreenCapture* capture = nullptr;
};
using Reader = std::shared_ptr<ReaderLink>;

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

    /// On a reader's thread, its link held: the newest picture, handed out.
    void take(ReaderLink& link) {
        AImage* acquired = nullptr;
        if (AImageReader_acquireLatestImage(link.reader, &acquired) != AMEDIA_OK || !acquired)
            return;
        Image image(acquired, AImage_delete);
        const Timestamp now = clock_.now();
        {
            std::lock_guard lock(mutex_);
            if (over_ || !pacer_.admit(now)) return;
        }
        if (options_.gpu) {
            hand_out(std::move(image), link.shared_from_this(), now);
        } else {
            convert(image.get(), now);
        }
    }

private:
    /// The screen mirrored into a reader of its size.
    Result<void> show(const android::Projection::Screen& screen) {
        const auto [width, height] = size_for(screen);
        auto reader = std::make_shared<ReaderLink>();
        const media_status_t made =
            options_.gpu ? AImageReader_newWithUsage(int32_t(width), int32_t(height),
                                                     AIMAGE_FORMAT_RGBA_8888, kGpuUsage,
                                                     kHeldGpuImages, &reader->reader)
                         : AImageReader_new(int32_t(width), int32_t(height),
                                            AIMAGE_FORMAT_RGBA_8888, kHeldImages, &reader->reader);
        if (made != AMEDIA_OK) return fail(Errc::Unavailable);
        reader->capture = this;
        AImageReader_ImageListener listener{reader.get(), &AndroidScreenCapture::on_image};
        AImageReader_setImageListener(reader->reader, &listener);
        ANativeWindow* window = nullptr;
        if (AImageReader_getWindow(reader->reader, &window) != AMEDIA_OK ||
            !projection_->show(window, width, height, screen.dpi)) {
            retire(std::move(reader));
            return fail(Errc::Unavailable);
        }
        LOG_INFO("engine", "the screen, " << screen.width << "x" << screen.height << ", as "
                                          << width << "x" << height
                                          << (options_.gpu ? " in GPU memory" : ""));
        // The reader before goes once the display draws into this one.
        std::swap(reader_, reader);
        if (reader) retire(std::move(reader));
        shown_ = screen;
        return {};
    }

    /// `reader` cut from the capture, once no callback of its is at work,
    /// and let go of: none reaches the capture any more. Its pictures may
    /// keep it a while yet.
    static void retire(Reader reader) {
        AImageReader_setImageListener(reader->reader, nullptr);
        std::lock_guard lock(reader->mutex);
        reader->capture = nullptr;
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

    /// The picture of `image`, read into the format asked for.
    void convert(AImage* image, Timestamp now) {
        AImageCropRect crop{};
        uint8_t* data = nullptr;
        int length = 0;
        int32_t row_step = 0;
        int32_t pixel_step = 0;
        if (AImage_getCropRect(image, &crop) != AMEDIA_OK ||
            AImage_getPlaneData(image, 0, &data, &length) != AMEDIA_OK ||
            AImage_getPlaneRowStride(image, 0, &row_step) != AMEDIA_OK ||
            AImage_getPlanePixelStride(image, 0, &pixel_step) != AMEDIA_OK || !data ||
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

    /// The picture of `image` in its buffer, which it holds — with the image,
    /// and the reader the image goes back to — for as long as it lives.
    void hand_out(Image image, Reader reader, Timestamp now) {
        AImage* const held = image.release();
        std::shared_ptr<const void> owner(held, [reader = std::move(reader)](const void* kept) {
            AImage_delete(static_cast<AImage*>(const_cast<void*>(kept)));
        });
        VideoFrame picture =
            android::gpu_frame_of(held, std::move(owner), now, options_.color_space);
        std::lock_guard lock(mutex_);
        if (!picture.empty() && !over_) sink_(std::move(picture));
    }

    /// The end, said once.
    void end(Result<void> outcome) {
        std::lock_guard lock(mutex_);
        if (over_) return;
        over_ = true;
        if (ended_) ended_(std::move(outcome));
    }

    static void on_image(void* context, AImageReader*) {
        auto* link = static_cast<ReaderLink*>(context);
        std::lock_guard lock(link->mutex);
        if (link->capture) link->capture->take(*link);
    }

    const ScreenCaptureOptions options_;
    const std::shared_ptr<android::Projection> projection_;
    Clock& clock_;
    const std::string no_token_;  ///< Android keeps no consent

    std::mutex start_mutex_;  ///< serialises start() and stop()
    Reader reader_;           ///< the one the display draws into: start()'s, then the watcher's
    android::Projection::Screen shown_;
    std::jthread watcher_;

    std::mutex mutex_;  ///< over the sink, the end and the pacer
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
