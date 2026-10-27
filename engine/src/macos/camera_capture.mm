// Cameras on macOS: AVFoundation's video devices — built in, external,
// Continuity — each known by its unique id, which a node is a hash of. The
// mode taken is the first the camera lists no larger than asked at 15
// pictures a second or more, else the first no larger, else its first, of
// those in a format read here (4:2:0, YUY2, UYVY, BGRA): compressed ones are
// not taken. The session hands its pictures out as NV12 in the camera's own
// colour space, each held by the frame that carries it (pixel_buffer.h); I420
// is converted from NV12. The camera ends the capture with success as it is
// unplugged, with a failure on the session's error. The system asks the
// user's leave for the camera the first time: start() waits for the answer.
// Built with ARC.

#include "capture/camera_backend.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <dispatch/dispatch.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture/frame_pacer.h"
#include "macos/host_time.h"
#include "macos/pixel_buffer.h"
#include "ratcast/engine/video_convert.h"

#include "librats/util/logger.h"

namespace ratcast::macos {
class CameraStream;
}

/// What a session hands out and says, handed to its CameraStream on the
/// stream's queue until detached there.
@interface RCCameraOutput : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
- (instancetype)initWithStream:(ratcast::macos::CameraStream*)stream queue:(dispatch_queue_t)queue;
/// On the queue: nothing more is handed on.
- (void)detach;
/// From any thread: the session ended, with success when the camera went.
- (void)endedWithSuccess:(BOOL)success;
@end

namespace ratcast::macos {
namespace {

constexpr double kSmoothRate = 15;
constexpr int64_t kLeavePatienceMs = 60'000;

std::string utf8(NSString* text) { return text ? std::string(text.UTF8String) : std::string(); }

/// A camera's node: a hash of its unique id (FNV-1a), never 0.
uint32_t node_of(NSString* unique_id) {
    uint32_t hash = 2166136261u;
    for (const char* c = unique_id.UTF8String; c && *c; ++c) {
        hash ^= uint8_t(*c);
        hash *= 16777619u;
    }
    return hash != 0 ? hash : 1;
}

NSArray<AVCaptureDevice*>* camera_devices() {
    NSArray<AVCaptureDeviceType>* types = @[
        AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal,
        AVCaptureDeviceTypeContinuityCamera
    ];
    return [AVCaptureDeviceDiscoverySession
               discoverySessionWithDeviceTypes:types
                                     mediaType:AVMediaTypeVideo
                                      position:AVCaptureDevicePositionUnspecified]
        .devices;
}

AVCaptureDevice* device_of(uint32_t node) {
    for (AVCaptureDevice* device in camera_devices()) {
        if (node_of(device.uniqueID) == node) return device;
    }
    return nil;
}

/// A mode a camera lists, of a format read here.
struct Mode {
    AVCaptureDeviceFormat* format = nil;
    uint32_t width = 0;
    uint32_t height = 0;
    double frame_rate = 0;  ///< pictures a second, at most
};

std::vector<Mode> modes_of(AVCaptureDevice* device) {
    std::vector<Mode> modes;
    for (AVCaptureDeviceFormat* format in device.formats) {
        const FourCharCode type = CMFormatDescriptionGetMediaSubType(format.formatDescription);
        if (type != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
            type != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
            type != kCVPixelFormatType_422YpCbCr8_yuvs && type != kCVPixelFormatType_422YpCbCr8 &&
            type != kCVPixelFormatType_32BGRA) {
            continue;
        }
        const CMVideoDimensions size =
            CMVideoFormatDescriptionGetDimensions(format.formatDescription);
        Mode mode{format, uint32_t(std::max(size.width, 0)), uint32_t(std::max(size.height, 0))};
        for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges)
            mode.frame_rate = std::max(mode.frame_rate, range.maxFrameRate);
        modes.push_back(mode);
    }
    return modes;
}

std::optional<size_t> choose_mode(const std::vector<Mode>& modes, uint32_t max_width,
                                  uint32_t max_height) {
    const auto fits = [&](const Mode& mode) {
        return mode.width <= max_width && mode.height <= max_height;
    };
    for (size_t i = 0; i < modes.size(); ++i) {
        if (fits(modes[i]) && modes[i].frame_rate >= kSmoothRate) return i;
    }
    for (size_t i = 0; i < modes.size(); ++i) {
        if (fits(modes[i])) return i;
    }
    if (!modes.empty()) return 0;
    return std::nullopt;
}

/// Whether the user lets this process use the camera, asking if not yet.
bool camera_allowed() {
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
        case AVAuthorizationStatusAuthorized: return true;
        case AVAuthorizationStatusNotDetermined: break;
        default: return false;
    }
    __block BOOL granted = NO;
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL allowed) {
                                 granted = allowed;
                                 dispatch_semaphore_signal(answered);
                             }];
    dispatch_semaphore_wait(answered,
                            dispatch_time(DISPATCH_TIME_NOW, kLeavePatienceMs * 1'000'000));
    return granted;
}

/// Set the camera to `mode` at no more than `max_rate` pictures a second.
/// Precondition: the device is locked for configuration.
void set_mode(AVCaptureDevice* device, const Mode& mode, uint32_t max_rate) {
    device.activeFormat = mode.format;
    // The range with the most pictures up to the rate asked; at its own most
    // (exactly its shortest duration, which a computed one may miss), or at
    // the rate asked where that lies inside it.
    AVFrameRateRange* best = nil;
    for (AVFrameRateRange* range in mode.format.videoSupportedFrameRateRanges) {
        if (range.minFrameRate > max_rate) continue;
        if (!best || std::min<double>(range.maxFrameRate, max_rate) >
                         std::min<double>(best.maxFrameRate, max_rate)) {
            best = range;
        }
    }
    if (!best) return;
    @try {
        device.activeVideoMinFrameDuration = best.maxFrameRate <= max_rate
                                                 ? best.minFrameDuration
                                                 : CMTimeMake(1, int32_t(max_rate));
    } @catch (NSException* exception) {
        LOG_WARN("engine", "the camera keeps its own rate: " << utf8(exception.reason));
    }
}

}  // namespace

/// One start()'s session, from start to stop.
class CameraStream {
public:
    CameraStream(AVCaptureDevice* device, const CameraOptions& options, Clock& clock,
                 VideoSource::Sink sink, VideoSource::EndHandler ended)
        : device_(device),
          options_(options),
          clock_(clock),
          sink_(std::move(sink)),
          ended_(std::move(ended)),
          pacer_(options.max_frame_rate),
          queue_(dispatch_queue_create("ratcast.camera", DISPATCH_QUEUE_SERIAL)) {}

    CameraStream(const CameraStream&) = delete;
    CameraStream& operator=(const CameraStream&) = delete;

    ~CameraStream() {
        for (id observer : observers_) [NSNotificationCenter.defaultCenter removeObserver:observer];
        if (session_.running) [session_ stopRunning];
        RCCameraOutput* output = output_;
        dispatch_sync(queue_, ^{ [output detach]; });
    }

    Result<void> start() {
        if (!camera_allowed()) {
            LOG_WARN("engine", "the camera is not this application's to use");
            return fail(Errc::Unavailable);
        }
        const std::vector<Mode> modes = modes_of(device_);
        const std::optional<size_t> chosen =
            choose_mode(modes, options_.max_width, options_.max_height);
        if (!chosen) {
            LOG_WARN("engine", "the camera sends pictures in no format read here");
            return fail(Errc::Unsupported);
        }
        const Mode& mode = modes[*chosen];

        NSError* error = nil;
        AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device_
                                                                            error:&error];
        if (!input) {
            LOG_WARN("engine", "cannot open the camera: " << utf8(error.localizedDescription));
            return fail(Errc::Unavailable);
        }
        output_ = [[RCCameraOutput alloc] initWithStream:this queue:queue_];
        AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
        output.videoSettings = @{
            (id)
            kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        };
        output.alwaysDiscardsLateVideoFrames = YES;
        [output setSampleBufferDelegate:output_ queue:queue_];

        session_ = [[AVCaptureSession alloc] init];
        [session_ beginConfiguration];
        if (![session_ canAddInput:input] || ![session_ canAddOutput:output]) {
            [session_ commitConfiguration];
            return fail(Errc::Unavailable);
        }
        [session_ addInput:input];
        [session_ addOutput:output];
        // Locked until the session runs, so that it keeps the mode set.
        const bool locked = [device_ lockForConfiguration:&error];
        if (locked) set_mode(device_, mode, options_.max_frame_rate);
        [session_ commitConfiguration];
        watch();
        [session_ startRunning];
        if (locked) [device_ unlockForConfiguration];
        if (!session_.running) {
            LOG_WARN("engine", "the camera did not start");
            return fail(Errc::Unavailable);
        }
        LOG_INFO("engine", "camera: " << mode.width << "x" << mode.height << " at "
                                      << mode.frame_rate << " a second at most");
        return {};
    }

    /// On the queue: one picture of the camera's.
    void take(CMSampleBufferRef buffer) {
        CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(buffer);
        if (!pixels) return;
        const Timestamp now = clock_.now();
        if (!pacer_.admit(now)) return;
        VideoFrame picture = frame_of(pixels, captured(buffer, now), color_space_of(pixels));
        if (picture.empty() || picture.format() != PixelFormat::NV12) return;
        if (options_.format == PixelFormat::I420) {
            Result<VideoFrame> converted =
                convert_frame(picture, PixelFormat::I420, picture.color_space());
            if (!converted) return;
            picture = std::move(*converted);
        }
        sink_(std::move(picture));
    }

    /// On the queue: the end, said once.
    void end(Result<void> outcome) {
        if (over_) return;
        over_ = true;
        if (ended_) ended_(std::move(outcome));
    }

private:
    /// When `buffer` was captured, by the session's clock turned to the host's.
    Timestamp captured(CMSampleBufferRef buffer, Timestamp now) const {
        const CMTime stamp = CMSampleBufferGetPresentationTimeStamp(buffer);
        CMClockRef clock = session_.synchronizationClock;
        if (!CMTIME_IS_NUMERIC(stamp) || !clock) return now;
        const CMTime host = CMSyncConvertTime(stamp, clock, CMClockGetHostTimeClock());
        if (!CMTIME_IS_NUMERIC(host)) return now;
        return std::min(now, at_host_time(clock_, CMClockConvertHostTimeToSystemUnits(host)));
    }

    /// The camera unplugged, the session failing: the capture's end.
    void watch() {
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        RCCameraOutput* output = output_;
        observers_.push_back([center
            addObserverForName:AVCaptureDeviceWasDisconnectedNotification
                        object:device_
                         queue:nil
                    usingBlock:^(NSNotification*) { [output endedWithSuccess:YES]; }]);
        observers_.push_back([center
            addObserverForName:AVCaptureSessionRuntimeErrorNotification
                        object:session_
                         queue:nil
                    usingBlock:^(NSNotification* note) {
                        NSError* error = note.userInfo[AVCaptureSessionErrorKey];
                        LOG_WARN("engine",
                                 "the camera failed: " << utf8(error.localizedDescription));
                        [output endedWithSuccess:NO];
                    }]);
    }

    AVCaptureDevice* const device_;
    const CameraOptions options_;
    Clock& clock_;
    const VideoSource::Sink sink_;
    const VideoSource::EndHandler ended_;

    // The queue's, once started.
    FramePacer pacer_;
    bool over_ = false;

    dispatch_queue_t queue_;
    RCCameraOutput* output_ = nil;
    AVCaptureSession* session_ = nil;
    std::vector<id> observers_;
};

namespace {

class MacCamera final : public VideoSource {
public:
    MacCamera(const CameraOptions& options, Clock& clock) : options_(options), clock_(clock) {}
    ~MacCamera() override { stop(); }

    Result<void> start(Sink sink, EndHandler ended) override {
        std::lock_guard lock(mutex_);
        if (stream_ || missing_.joinable()) return fail(Errc::InvalidArgument);
        AVCaptureDevice* device = device_of(options_.node);
        if (!device) {
            LOG_WARN("engine", "camera " << options_.node << " is not there");
            missing_ = std::thread([ended = std::move(ended)] {
                if (ended) ended(fail(Errc::Unavailable));
            });
            return {};
        }
        auto stream = std::make_unique<CameraStream>(device, options_, clock_, std::move(sink),
                                                     std::move(ended));
        if (Result<void> started = stream->start(); !started) return started;
        stream_ = std::move(stream);
        return {};
    }

    void stop() override {
        std::lock_guard lock(mutex_);
        stream_.reset();
        if (missing_.joinable()) missing_.join();
    }

private:
    const CameraOptions options_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    std::unique_ptr<CameraStream> stream_;
    std::thread missing_;  ///< saying a camera that is not there ended
};

}  // namespace
}  // namespace ratcast::macos

@implementation RCCameraOutput {
    ratcast::macos::CameraStream* _stream;  // the queue's
    dispatch_queue_t _queue;
}

- (instancetype)initWithStream:(ratcast::macos::CameraStream*)stream queue:(dispatch_queue_t)queue {
    if ((self = [super init])) {
        _stream = stream;
        _queue = queue;
    }
    return self;
}

- (void)detach {
    _stream = nullptr;
}

- (void)endedWithSuccess:(BOOL)success {
    dispatch_async(_queue, ^{
        if (!self->_stream) return;
        if (success)
            self->_stream->end(ratcast::Result<void>{});
        else
            self->_stream->end(ratcast::fail(ratcast::Errc::Internal));
    });
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)buffer
           fromConnection:(AVCaptureConnection*)connection {
    if (_stream) _stream->take(buffer);
}

@end

namespace ratcast::backend {

Result<std::vector<CameraInfo>> list_cameras() {
    std::vector<CameraInfo> cameras;
    for (AVCaptureDevice* device in macos::camera_devices()) {
        cameras.push_back({macos::node_of(device.uniqueID), macos::utf8(device.uniqueID),
                           macos::utf8(device.localizedName)});
    }
    return cameras;
}

Result<std::unique_ptr<VideoSource>> open_camera(const CameraOptions& options) {
    CameraOptions resolved = options;
    if (resolved.node == 0) {
        NSArray<AVCaptureDevice*>* devices = macos::camera_devices();
        if (devices.count == 0) return fail(Errc::Unavailable);
        resolved.node = macos::node_of(devices.firstObject.uniqueID);
    }
    return std::unique_ptr<VideoSource>(
        std::make_unique<macos::MacCamera>(resolved, SteadyClock::instance()));
}

}  // namespace ratcast::backend
