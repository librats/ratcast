// Screen capture on macOS: ScreenCaptureKit. The user chooses in the system's
// picker (SCContentSharingPicker, macOS 14), which needs no leave to record
// the screen beyond the choice; a restore token names a screen by its
// display's UUID, a window by its application's bundle id, and is found
// again among what ScreenCaptureKit lists — which does need that leave. A
// process with no application (the command-line tools, tests) cannot show
// the picker: it captures the main screen, as a token would.
//
// Pictures come as the stream's pixel buffers, NV12 in the colour space asked
// (or BGRA), each held by the frame that carries it (pixel_buffer.h) — no
// copy; asked for GPU pictures, NV12 ones as the IOSurfaces they are, which
// VideoToolbox reads where they are. I420 is converted from NV12. A window's
// capture follows its size. Built with ARC.

#include "ratcast/engine/screen_capture.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "capture/frame_pacer.h"
#include "macos/host_time.h"
#include "macos/pixel_buffer.h"
#include "ratcast/engine/video_convert.h"

#include "librats/util/logger.h"

namespace ratcast::macos {
class ScreenStream;
}

/// What a stream says, handed to its ScreenStream on the stream's queue
/// until detached there.
@interface RCScreenStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
- (instancetype)initWithStream:(ratcast::macos::ScreenStream*)stream queue:(dispatch_queue_t)queue;
/// On the queue: nothing more is handed on.
- (void)detach;
@end

/// What the picker says, once.
@interface RCPickerObserver : NSObject <SCContentSharingPickerObserver>
@property(atomic, strong) SCContentFilter* filter;
@property(atomic) BOOL failed;
/// Whether the picker has said, within `milliseconds`.
- (BOOL)waitFor:(int64_t)milliseconds;
@end

namespace ratcast::macos {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kDisplayToken = "display:";
constexpr std::string_view kWindowToken = "window:";
constexpr int64_t kStartPatienceMs = 10'000;
constexpr int64_t kStopPatienceMs = 5'000;

std::string utf8(NSString* text) { return text ? std::string(text.UTF8String) : std::string(); }

uint32_t even(double size) { return uint32_t(std::max(0.0, std::round(size))) & ~1u; }

/// What names a display from one run to the next.
std::string display_uuid(CGDirectDisplayID display) {
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(display);
    if (!uuid) return {};
    CFStringRef text = CFUUIDCreateString(nullptr, uuid);
    CFRelease(uuid);
    if (!text) return {};
    std::string out = utf8((__bridge NSString*)text);
    CFRelease(text);
    return out;
}

std::string token_of(SCDisplay* display) {
    const std::string uuid = display_uuid(display.displayID);
    return uuid.empty() ? std::string() : std::string(kDisplayToken) + uuid;
}

std::string token_of(SCWindow* window) {
    const std::string bundle = utf8(window.owningApplication.bundleIdentifier);
    return bundle.empty() ? std::string() : std::string(kWindowToken) + bundle;
}

/// What ScreenCaptureKit lists now: nil without the leave to record the
/// screen (the system asks for it once).
SCShareableContent* shareable_content() {
    __block SCShareableContent* content = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:YES
                               onScreenWindowsOnly:YES
                                 completionHandler:^(SCShareableContent* found, NSError* error) {
                                     if (error)
                                         LOG_WARN("engine", "cannot list the screens: " << utf8(
                                                                error.localizedDescription));
                                     content = found;
                                     dispatch_semaphore_signal(done);
                                 }];
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, kStartPatienceMs * 1'000'000));
    return content;
}

/// A capture's subject: what to capture, how large, and its token.
struct Subject {
    SCContentFilter* filter = nil;
    uint32_t width = 0;
    uint32_t height = 0;
    bool window = false;
    std::string token;
};

Subject subject_of(SCDisplay* display) {
    Subject subject;
    subject.filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
    subject.token = token_of(display);
    return subject;
}

Subject subject_of(SCWindow* window) {
    Subject subject;
    subject.filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
    subject.window = true;
    subject.token = token_of(window);
    return subject;
}

/// The size in pixels of what `subject.filter` shows.
void measure(Subject& subject) {
    const CGRect rect = subject.filter.contentRect;
    const double scale = std::max<double>(subject.filter.pointPixelScale, 1);
    subject.width = even(rect.size.width * scale);
    subject.height = even(rect.size.height * scale);
}

/// What `token` names among `content`: the screen of that display, or the
/// largest window of that application. Null filter when it is not there.
Subject find(SCShareableContent* content, std::string_view token,
             const ScreenCaptureOptions& options) {
    if (options.monitors && token.starts_with(kDisplayToken)) {
        for (SCDisplay* display in content.displays) {
            if (display_uuid(display.displayID) == token.substr(kDisplayToken.size()))
                return subject_of(display);
        }
    }
    if (options.windows && token.starts_with(kWindowToken)) {
        SCWindow* best = nil;
        double best_area = 0;
        for (SCWindow* window in content.windows) {
            if (window.windowLayer != 0 || !window.onScreen) continue;
            if (utf8(window.owningApplication.bundleIdentifier) !=
                token.substr(kWindowToken.size()))
                continue;
            const double area = window.frame.size.width * window.frame.size.height;
            if (area > best_area) {
                best = window;
                best_area = area;
            }
        }
        if (best) return subject_of(best);
    }
    return {};
}

/// Whether the picker can be shown: there is an application, whose main
/// thread serves the main queue (Qt's does; a command-line tool's has none,
/// and nothing would serve it).
bool can_pick() { return NSApp != nil; }

std::mutex picking;  ///< the picker is the system's one: one choice at a time

/// The user's choice in the system's picker. Fails with Cancelled when the
/// user declined or `stop` was requested.
Result<Subject> pick(const ScreenCaptureOptions& options, const std::stop_token& stop) {
    std::lock_guard lock(picking);
    RCPickerObserver* observer = [[RCPickerObserver alloc] init];
    SCContentSharingPickerConfiguration* configuration =
        [[SCContentSharingPickerConfiguration alloc] init];
    SCContentSharingPickerMode modes = 0;
    if (options.monitors) modes |= SCContentSharingPickerModeSingleDisplay;
    if (options.windows) modes |= SCContentSharingPickerModeSingleWindow;
    configuration.allowedPickerModes = modes;
    configuration.allowsChangingSelectedContent = NO;
    dispatch_async(dispatch_get_main_queue(), ^{
        SCContentSharingPicker* picker = SCContentSharingPicker.sharedPicker;
        picker.defaultConfiguration = configuration;
        [picker addObserver:observer];
        picker.active = YES;
        [picker present];
    });
    bool stopped = false;
    while (![observer waitFor:100]) {
        if (stop.stop_requested()) {
            stopped = true;
            break;
        }
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        SCContentSharingPicker* picker = SCContentSharingPicker.sharedPicker;
        [picker removeObserver:observer];
        picker.active = NO;
    });
    if (stopped) return fail(Errc::Cancelled);
    if (observer.failed) return fail(Errc::Unavailable);
    if (!observer.filter) return fail(Errc::Cancelled);

    Subject subject;
    subject.filter = observer.filter;
    subject.window = observer.filter.style == SCShareableContentStyleWindow;
    // What was chosen is told from macOS 15.2 on: before, no token.
    if (@available(macOS 15.2, *)) {
        if (SCDisplay* display = observer.filter.includedDisplays.firstObject)
            subject.token = token_of(display);
        else if (SCWindow* window = observer.filter.includedWindows.firstObject)
            subject.token = token_of(window);
    }
    return subject;
}

}  // namespace

/// One start()'s stream, from start to stop.
class ScreenStream {
public:
    ScreenStream(const Subject& subject, const ScreenCaptureOptions& options, Clock& clock,
                 VideoSource::Sink sink, VideoSource::EndHandler ended)
        : options_(options),
          clock_(clock),
          sink_(std::move(sink)),
          ended_(std::move(ended)),
          filter_(subject.filter),
          window_(subject.window),
          width_(subject.width),
          height_(subject.height),
          pacer_(options.max_frame_rate),
          queue_(dispatch_queue_create("ratcast.screen-capture", DISPATCH_QUEUE_SERIAL)) {}

    ScreenStream(const ScreenStream&) = delete;
    ScreenStream& operator=(const ScreenStream&) = delete;

    ~ScreenStream() {
        if (started_) {
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [stream_
                stopCaptureWithCompletionHandler:^(NSError*) { dispatch_semaphore_signal(done); }];
            dispatch_semaphore_wait(done,
                                    dispatch_time(DISPATCH_TIME_NOW, kStopPatienceMs * 1'000'000));
        }
        RCScreenStreamOutput* output = output_;
        dispatch_sync(queue_, ^{ [output detach]; });
    }

    Result<void> start() {
        output_ = [[RCScreenStreamOutput alloc] initWithStream:this queue:queue_];
        stream_ = [[SCStream alloc] initWithFilter:filter_
                                     configuration:configuration(width_, height_)
                                          delegate:output_];
        NSError* error = nil;
        if (![stream_ addStreamOutput:output_
                                 type:SCStreamOutputTypeScreen
                   sampleHandlerQueue:queue_
                                error:&error]) {
            LOG_WARN("engine", "cannot capture the screen: " << utf8(error.localizedDescription));
            return fail(Errc::Internal);
        }
        __block NSError* failed = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [stream_ startCaptureWithCompletionHandler:^(NSError* outcome) {
            failed = outcome;
            dispatch_semaphore_signal(done);
        }];
        if (dispatch_semaphore_wait(
                done, dispatch_time(DISPATCH_TIME_NOW, kStartPatienceMs * 1'000'000)) != 0) {
            LOG_WARN("engine", "the screen capture did not start");
            started_ = true;  // stopped all the same
            return fail(Errc::Unavailable);
        }
        if (failed) {
            LOG_WARN("engine", "cannot capture the screen: " << utf8(failed.localizedDescription));
            return fail(failed.code == SCStreamErrorUserDeclined ? Errc::Cancelled
                                                                 : Errc::Unavailable);
        }
        started_ = true;
        return {};
    }

    /// On the queue: one picture of the stream's.
    void take(CMSampleBufferRef buffer) {
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(buffer, false);
        if (!attachments || CFArrayGetCount(attachments) == 0) return;
        NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
        NSNumber* status = info[SCStreamFrameInfoStatus];
        if (!status) return;
        switch (SCFrameStatus(status.integerValue)) {
            case SCFrameStatusComplete: break;
            case SCFrameStatusStopped: end(Result<void>{}); return;
            default: return;  // nothing new on the screen
        }
        CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(buffer);
        if (!pixels) return;
        const Timestamp now = clock_.now();
        if (!pacer_.admit(now)) return;
        Timestamp captured = now;
        if (NSNumber* shown = info[SCStreamFrameInfoDisplayTime])
            captured = std::min(now, at_host_time(clock_, shown.unsignedLongLongValue));
        if (window_) follow_size(info);

        // Left on its IOSurface where it may be, for VideoToolbox to read there.
        VideoFrame picture;
        if (options_.gpu && options_.format == PixelFormat::NV12)
            picture = gpu_frame_of(pixels, captured, options_.color_space);
        if (picture.empty()) picture = frame_of(pixels, captured, options_.color_space);
        const PixelFormat sent =
            options_.format == PixelFormat::BGRA ? PixelFormat::BGRA : PixelFormat::NV12;
        if (picture.empty() || picture.format() != sent) return;
        if (options_.format == PixelFormat::I420) {
            Result<VideoFrame> converted =
                convert_frame(picture, PixelFormat::I420, options_.color_space);
            if (!converted) return;
            picture = std::move(*converted);
        }
        sink_(std::move(picture));
    }

    /// On the queue: the stream stopped by itself.
    void stopped_by_system(NSInteger code) {
        if (code == SCStreamErrorUserStopped) {
            end(Result<void>{});
        } else {
            LOG_WARN("engine", "the screen capture stopped: error " << code);
            end(fail(Errc::Internal));
        }
    }

private:
    SCStreamConfiguration* configuration(uint32_t width, uint32_t height) const {
        SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
        configuration.width = width;
        configuration.height = height;
        configuration.minimumFrameInterval = options_.max_frame_rate > 0
                                                 ? CMTimeMake(1, int32_t(options_.max_frame_rate))
                                                 : kCMTimeZero;
        configuration.showsCursor = options_.cursor;
        configuration.queueDepth = 6;
        configuration.colorSpaceName = kCGColorSpaceSRGB;
        if (options_.format == PixelFormat::BGRA) {
            configuration.pixelFormat = kCVPixelFormatType_32BGRA;
        } else {
            configuration.pixelFormat = options_.color_space.range == ColorRange::Full
                                            ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                            : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
            configuration.colorMatrix = options_.color_space.matrix == ColorMatrix::BT601
                                            ? kCVImageBufferYCbCrMatrix_ITU_R_601_4
                                            : kCVImageBufferYCbCrMatrix_ITU_R_709_2;
        }
        return configuration;
    }

    /// A window's pictures as large as it is now, in pixels.
    void follow_size(NSDictionary* info) {
        CFDictionaryRef rect_dictionary =
            (__bridge CFDictionaryRef)info[SCStreamFrameInfoContentRect];
        NSNumber* points_to_pixels = info[SCStreamFrameInfoScaleFactor];
        NSNumber* content_scale = info[SCStreamFrameInfoContentScale];
        CGRect rect{};
        if (!rect_dictionary || !points_to_pixels || !content_scale ||
            content_scale.doubleValue <= 0 ||
            !CGRectMakeWithDictionaryRepresentation(rect_dictionary, &rect)) {
            return;
        }
        const double scale = points_to_pixels.doubleValue / content_scale.doubleValue;
        const uint32_t width = even(rect.size.width * scale);
        const uint32_t height = even(rect.size.height * scale);
        if (width < 2 || height < 2 || (width == width_ && height == height_)) return;
        width_ = width;
        height_ = height;
        [stream_ updateConfiguration:configuration(width, height) completionHandler:nil];
    }

    /// On the queue: the end, said once.
    void end(Result<void> outcome) {
        if (over_) return;
        over_ = true;
        if (ended_) ended_(std::move(outcome));
    }

    const ScreenCaptureOptions options_;
    Clock& clock_;
    const VideoSource::Sink sink_;
    const VideoSource::EndHandler ended_;
    SCContentFilter* const filter_;
    const bool window_;

    // The queue's, once started.
    uint32_t width_;
    uint32_t height_;
    FramePacer pacer_;
    bool over_ = false;

    dispatch_queue_t queue_;
    RCScreenStreamOutput* output_ = nil;
    SCStream* stream_ = nil;
    bool started_ = false;
};

namespace {

class MacScreenCapture final : public ScreenCapture {
public:
    MacScreenCapture(Subject subject, const ScreenCaptureOptions& options, Clock& clock)
        : subject_(std::move(subject)), options_(options), clock_(clock) {}
    ~MacScreenCapture() override { stop(); }

    Result<void> start(Sink sink, EndHandler ended) override {
        std::lock_guard lock(mutex_);
        if (stream_) return fail(Errc::InvalidArgument);
        auto stream = std::make_unique<ScreenStream>(subject_, options_, clock_, std::move(sink),
                                                     std::move(ended));
        if (Result<void> started = stream->start(); !started) return started;
        stream_ = std::move(stream);
        return {};
    }

    void stop() override {
        std::lock_guard lock(mutex_);
        stream_.reset();
    }

    const std::string& restore_token() const noexcept override { return subject_.token; }

private:
    const Subject subject_;
    const ScreenCaptureOptions options_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    std::unique_ptr<ScreenStream> stream_;
};

}  // namespace
}  // namespace ratcast::macos

@implementation RCScreenStreamOutput {
    ratcast::macos::ScreenStream* _stream;  // the queue's
    dispatch_queue_t _queue;
}

- (instancetype)initWithStream:(ratcast::macos::ScreenStream*)stream queue:(dispatch_queue_t)queue {
    if ((self = [super init])) {
        _stream = stream;
        _queue = queue;
    }
    return self;
}

- (void)detach {
    _stream = nullptr;
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)buffer
                   ofType:(SCStreamOutputType)type {
    if (type == SCStreamOutputTypeScreen && _stream) _stream->take(buffer);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    const NSInteger code = error.code;
    dispatch_async(_queue, ^{
        if (self->_stream) self->_stream->stopped_by_system(code);
    });
}

@end

@implementation RCPickerObserver {
    dispatch_semaphore_t _said;
}

- (instancetype)init {
    if ((self = [super init])) _said = dispatch_semaphore_create(0);
    return self;
}

- (BOOL)waitFor:(int64_t)milliseconds {
    return dispatch_semaphore_wait(_said,
                                   dispatch_time(DISPATCH_TIME_NOW, milliseconds * 1'000'000)) == 0;
}

- (void)contentSharingPicker:(SCContentSharingPicker*)picker
         didUpdateWithFilter:(SCContentFilter*)filter
                   forStream:(SCStream*)stream {
    if (stream) return;  // a stream's content changed: not a choice of this one
    self.filter = filter;
    dispatch_semaphore_signal(_said);
}

- (void)contentSharingPicker:(SCContentSharingPicker*)picker didCancelForStream:(SCStream*)stream {
    if (!stream) dispatch_semaphore_signal(_said);
}

- (void)contentSharingPickerStartDidFailWithError:(NSError*)error {
    LOG_WARN("engine",
             "the screen picker failed: " << ratcast::macos::utf8(error.localizedDescription));
    self.failed = YES;
    dispatch_semaphore_signal(_said);
}

@end

namespace ratcast {

Result<std::unique_ptr<ScreenCapture>> open_screen_capture(const ScreenCaptureOptions& options,
                                                           std::stop_token stop) {
    using namespace macos;
    if (!options.monitors && !options.windows) return fail(Errc::InvalidArgument);
    if (stop.stop_requested()) return fail(Errc::Cancelled);

    Subject subject;
    SCShareableContent* content = nil;
    if (!options.restore_token.empty()) {
        content = shareable_content();
        if (content) subject = find(content, options.restore_token, options);
    }
    if (!subject.filter) {
        if (can_pick()) {
            Result<Subject> picked = pick(options, stop);
            if (!picked) return fail(picked.error());
            subject = std::move(*picked);
        } else if (options.monitors) {
            // No picker to show: the main screen, as the command-line tools have it.
            if (!content) content = shareable_content();
            for (SCDisplay* display in content.displays) {
                if (display.displayID == CGMainDisplayID()) subject = subject_of(display);
            }
            if (!subject.filter) return fail(Errc::Unavailable);
        } else {
            return fail(Errc::Unavailable);
        }
    }
    measure(subject);
    if (subject.width < 2 || subject.height < 2) return fail(Errc::Unavailable);
    return std::unique_ptr<ScreenCapture>(
        std::make_unique<MacScreenCapture>(std::move(subject), options, SteadyClock::instance()));
}

}  // namespace ratcast
