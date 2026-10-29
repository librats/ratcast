// Cameras on Android: Camera2 through the NDK. Those listed are the ones
// that make pictures as cameras do (BACKWARD_COMPATIBLE: not depth sensors),
// the front ones first — a call's camera — then the back and external ones,
// each known by its camera id, which a node is a hash of. The size taken is
// the largest of YUV_420_888's no larger than asked, as the sensor sees it,
// at 15 pictures a second or more, else the largest no larger, else the
// smallest; the rate the auto-exposure's range with the most pictures up to
// the rate asked. Pictures come through an AImageReader, on its own thread,
// turned upright — by the sensor's orientation and the display's rotation,
// looked at twice a second — into NV12 or I420 (camera_picture.h), in JFIF's
// colour space (BT.601, full range). A camera taken away (by another
// application, or with the app in the background without a foreground
// service of the camera's type) ends the capture with success, one that
// fails with a failure. Needs CAMERA, which the application asks for.

#include "capture/camera_backend.h"

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "android/camera_picture.h"
#include "android/java.h"
#include "capture/frame_pacer.h"

#include "librats/util/logger.h"

namespace ratcast::android {
namespace {

constexpr int32_t kSmoothRate = 15;
/// How often the display's rotation is looked at.
constexpr Duration kRotationEvery = std::chrono::milliseconds(500);
/// Pictures the reader holds: the one read, the one coming, one spare.
constexpr int32_t kHeldImages = 3;

struct ManagerDeleter {
    void operator()(ACameraManager* manager) const { ACameraManager_delete(manager); }
};
using Manager = std::unique_ptr<ACameraManager, ManagerDeleter>;

struct MetadataDeleter {
    void operator()(ACameraMetadata* metadata) const { ACameraMetadata_free(metadata); }
};
using Metadata = std::unique_ptr<ACameraMetadata, MetadataDeleter>;

/// A camera's node: a hash of its id (FNV-1a), never 0.
uint32_t node_of(const std::string& id) {
    uint32_t hash = 2166136261u;
    for (const char c : id) {
        hash ^= uint8_t(c);
        hash *= 16777619u;
    }
    return hash != 0 ? hash : 1;
}

/// What the system says of a camera, and what the engine takes of it.
struct Camera {
    std::string id;
    uint8_t facing = ACAMERA_LENS_FACING_EXTERNAL;
    int32_t orientation = 0;  ///< degrees clockwise that turn its pictures upright
    bool boot_time = false;   ///< its pictures stamped by CLOCK_BOOTTIME
    Metadata characteristics;
};

Metadata characteristics_of(ACameraManager* manager, const char* id) {
    ACameraMetadata* metadata = nullptr;
    if (ACameraManager_getCameraCharacteristics(manager, id, &metadata) != ACAMERA_OK)
        return nullptr;
    return Metadata(metadata);
}

/// The cameras there are, in the order they are listed.
std::vector<Camera> cameras_of(ACameraManager* manager) {
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK || !ids) return {};
    std::vector<Camera> cameras;
    for (int i = 0; i < ids->numCameras; ++i) {
        Metadata metadata = characteristics_of(manager, ids->cameraIds[i]);
        if (!metadata) continue;
        ACameraMetadata_const_entry entry{};
        bool pictures = false;
        if (ACameraMetadata_getConstEntry(metadata.get(), ACAMERA_REQUEST_AVAILABLE_CAPABILITIES,
                                          &entry) == ACAMERA_OK) {
            pictures =
                std::ranges::find(entry.data.u8, entry.data.u8 + entry.count,
                                  ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE) !=
                entry.data.u8 + entry.count;
        }
        if (!pictures) continue;
        Camera camera;
        camera.id = ids->cameraIds[i];
        if (ACameraMetadata_getConstEntry(metadata.get(), ACAMERA_LENS_FACING, &entry) ==
                ACAMERA_OK &&
            entry.count > 0) {
            camera.facing = entry.data.u8[0];
        }
        if (ACameraMetadata_getConstEntry(metadata.get(), ACAMERA_SENSOR_ORIENTATION, &entry) ==
                ACAMERA_OK &&
            entry.count > 0) {
            camera.orientation = entry.data.i32[0];
        }
        if (ACameraMetadata_getConstEntry(metadata.get(), ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE,
                                          &entry) == ACAMERA_OK &&
            entry.count > 0) {
            camera.boot_time = entry.data.u8[0] == ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
        }
        camera.characteristics = std::move(metadata);
        cameras.push_back(std::move(camera));
    }
    ACameraManager_deleteCameraIdList(ids);
    // The front ones first, then the back ones, then the rest; each as listed.
    const auto rank = [](const Camera& camera) {
        return camera.facing == ACAMERA_LENS_FACING_FRONT  ? 0
               : camera.facing == ACAMERA_LENS_FACING_BACK ? 1
                                                           : 2;
    };
    std::ranges::stable_sort(cameras, {}, rank);
    return cameras;
}

std::vector<CameraInfo> infos_of(const std::vector<Camera>& cameras) {
    std::vector<CameraInfo> infos;
    int counts[3] = {};
    for (const Camera& camera : cameras) {
        const int facing = std::min<int>(camera.facing, 2);
        const char* names[3] = {"Front camera", "Back camera", "External camera"};
        std::string name = names[facing];
        if (++counts[facing] > 1) name += " " + std::to_string(counts[facing]);
        infos.push_back({node_of(camera.id), camera.id, std::move(name)});
    }
    return infos;
}

/// A size YUV_420_888 comes in, and its most pictures a second.
struct Mode {
    int32_t width = 0;
    int32_t height = 0;
    double frame_rate = 0;
};

std::vector<Mode> modes_of(const ACameraMetadata* characteristics) {
    std::vector<Mode> modes;
    ACameraMetadata_const_entry entry{};
    if (ACameraMetadata_getConstEntry(characteristics,
                                      ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                      &entry) != ACAMERA_OK) {
        return modes;
    }
    for (uint32_t i = 0; i + 3 < entry.count; i += 4) {
        const int32_t* config = entry.data.i32 + i;
        if (config[0] == AIMAGE_FORMAT_YUV_420_888 &&
            config[3] == ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) {
            modes.push_back({config[1], config[2]});
        }
    }
    if (ACameraMetadata_getConstEntry(characteristics, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                                      &entry) == ACAMERA_OK) {
        for (uint32_t i = 0; i + 3 < entry.count; i += 4) {
            const int64_t* duration = entry.data.i64 + i;
            if (duration[0] != AIMAGE_FORMAT_YUV_420_888 || duration[3] <= 0) continue;
            for (Mode& mode : modes) {
                if (mode.width == duration[1] && mode.height == duration[2])
                    mode.frame_rate = 1e9 / double(duration[3]);
            }
        }
    }
    return modes;
}

std::optional<Mode> choose_mode(const std::vector<Mode>& modes, uint32_t max_width,
                                uint32_t max_height) {
    const auto area = [](const Mode& mode) { return int64_t(mode.width) * mode.height; };
    const auto fits = [&](const Mode& mode) {
        return uint32_t(mode.width) <= max_width && uint32_t(mode.height) <= max_height;
    };
    const auto largest = [&](auto&& admits) {
        std::optional<Mode> best;
        for (const Mode& mode : modes) {
            if (admits(mode) && (!best || area(mode) > area(*best))) best = mode;
        }
        return best;
    };
    if (auto mode = largest([&](const Mode& m) { return fits(m) && m.frame_rate >= kSmoothRate; }))
        return mode;
    if (auto mode = largest(fits)) return mode;
    if (modes.empty()) return std::nullopt;
    return *std::ranges::min_element(modes, {}, area);
}

/// The auto-exposure's range with the most pictures up to `max_rate`; of
/// those, the one that lets the fewest down to 15 (darker rooms exposed
/// longer), else the steadiest.
std::optional<std::pair<int32_t, int32_t>> choose_rates(const ACameraMetadata* characteristics,
                                                        uint32_t max_rate) {
    ACameraMetadata_const_entry entry{};
    if (ACameraMetadata_getConstEntry(characteristics,
                                      ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                                      &entry) != ACAMERA_OK) {
        return std::nullopt;
    }
    std::optional<std::pair<int32_t, int32_t>> best;
    const auto better = [](std::pair<int32_t, int32_t> range, std::pair<int32_t, int32_t> than) {
        if (range.second != than.second) return range.second > than.second;
        const bool smooth = range.first >= kSmoothRate;
        if (smooth != (than.first >= kSmoothRate)) return smooth;
        return smooth ? range.first < than.first : range.first > than.first;
    };
    for (uint32_t i = 0; i + 1 < entry.count; i += 2) {
        const std::pair range{entry.data.i32[i], entry.data.i32[i + 1]};
        if (range.second > int32_t(max_rate)) continue;
        if (!best || better(range, *best)) best = range;
    }
    return best;
}

/// The display's rotation in degrees: how far what is drawn is turned
/// (Surface.ROTATION_*), the other way to the device. 0 without Java.
int display_rotation() {
    JNIEnv* env = jni_env();
    jobject context = jni_context();
    if (!env || !context || env->PushLocalFrame(8) != JNI_OK) return 0;
    int degrees = 0;
    jclass context_type = env->GetObjectClass(context);
    jstring name = env->NewStringUTF("display");
    jobject displays =
        env->CallObjectMethod(context,
                              env->GetMethodID(context_type, "getSystemService",
                                               "(Ljava/lang/String;)Ljava/lang/Object;"),
                              name);
    if (!jni_failed(env) && displays) {
        jclass displays_type = env->GetObjectClass(displays);
        jobject display = env->CallObjectMethod(
            displays, env->GetMethodID(displays_type, "getDisplay", "(I)Landroid/view/Display;"),
            jint(0));
        if (!jni_failed(env) && display) {
            jclass display_type = env->GetObjectClass(display);
            const jint rotation =
                env->CallIntMethod(display, env->GetMethodID(display_type, "getRotation", "()I"));
            if (!jni_failed(env)) degrees = int(rotation & 3) * 90;
        }
    }
    env->PopLocalFrame(nullptr);
    return degrees;
}

/// CLOCK_BOOTTIME now, in nanoseconds: what Camera2 stamps pictures by where
/// its source is REALTIME.
int64_t boot_time_now() {
    timespec now{};
    clock_gettime(CLOCK_BOOTTIME, &now);
    return int64_t(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

const char* status_name(camera_status_t status) {
    switch (status) {
        case ACAMERA_ERROR_PERMISSION_DENIED: return "not this application's to use";
        case ACAMERA_ERROR_CAMERA_IN_USE: return "in use";
        case ACAMERA_ERROR_MAX_CAMERA_IN_USE: return "in use, as others are";
        case ACAMERA_ERROR_CAMERA_DISABLED: return "disabled";
        case ACAMERA_ERROR_CAMERA_DISCONNECTED: return "gone";
        default: return "failing";
    }
}

/// One start()'s camera, from start to stop: the device, its session and
/// the reader its pictures come through. Callbacks reach it by pointer
/// until it is closed.
class CameraStream {
public:
    CameraStream(Camera camera, const CameraOptions& options, Clock& clock, VideoSource::Sink sink,
                 VideoSource::EndHandler ended)
        : camera_(std::move(camera)),
          options_(options),
          clock_(clock),
          sink_(std::move(sink)),
          ended_(std::move(ended)),
          pacer_(options.max_frame_rate) {}

    CameraStream(const CameraStream&) = delete;
    CameraStream& operator=(const CameraStream&) = delete;

    ~CameraStream() { close(); }

    Result<void> start(ACameraManager* manager) {
        const std::vector<Mode> modes = modes_of(camera_.characteristics.get());
        const std::optional<Mode> mode =
            choose_mode(modes, options_.max_width, options_.max_height);
        if (!mode) {
            LOG_WARN("engine", "camera " << camera_.id << " makes no YUV pictures");
            return fail(Errc::Unsupported);
        }
        if (AImageReader_new(mode->width, mode->height, AIMAGE_FORMAT_YUV_420_888, kHeldImages,
                             &reader_) != AMEDIA_OK) {
            return fail(Errc::Unavailable);
        }
        AImageReader_setImageListener(reader_, &image_listener_);
        ANativeWindow* window = nullptr;
        if (AImageReader_getWindow(reader_, &window) != AMEDIA_OK) return fail(Errc::Unavailable);

        if (const camera_status_t opened = ACameraManager_openCamera(manager, camera_.id.c_str(),
                                                                     &device_callbacks_, &device_);
            opened != ACAMERA_OK) {
            LOG_WARN("engine", "camera " << camera_.id << " is " << status_name(opened));
            device_ = nullptr;
            return fail(Errc::Unavailable);
        }
        ACaptureRequest* request = nullptr;
        if (ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &request) != ACAMERA_OK)
            return fail(Errc::Unavailable);
        request_ = request;
        const std::optional<std::pair<int32_t, int32_t>> rates =
            choose_rates(camera_.characteristics.get(), options_.max_frame_rate);
        if (rates) {
            const int32_t range[2] = {rates->first, rates->second};
            ACaptureRequest_setEntry_i32(request_, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, range);
        }
        if (ACameraOutputTarget_create(window, &target_) != ACAMERA_OK ||
            ACaptureRequest_addTarget(request_, target_) != ACAMERA_OK ||
            ACaptureSessionOutputContainer_create(&outputs_) != ACAMERA_OK ||
            ACaptureSessionOutput_create(window, &output_) != ACAMERA_OK ||
            ACaptureSessionOutputContainer_add(outputs_, output_) != ACAMERA_OK) {
            return fail(Errc::Unavailable);
        }
        if (const camera_status_t made = ACameraDevice_createCaptureSession(
                device_, outputs_, &session_callbacks_, &session_);
            made != ACAMERA_OK) {
            LOG_WARN("engine", "camera " << camera_.id << ": no session, " << status_name(made));
            session_ = nullptr;
            return fail(Errc::Unavailable);
        }
        if (const camera_status_t repeating =
                ACameraCaptureSession_setRepeatingRequest(session_, nullptr, 1, &request_, nullptr);
            repeating != ACAMERA_OK) {
            LOG_WARN("engine",
                     "camera " << camera_.id << " does not start: " << status_name(repeating));
            return fail(Errc::Unavailable);
        }
        LOG_INFO("engine", "camera " << camera_.id << ": " << mode->width << "x" << mode->height
                                     << ", " << (rates ? rates->first : 0) << "-"
                                     << (rates ? rates->second : 0) << " a second, turned by its "
                                     << camera_.orientation);
        return {};
    }

    /// Everything closed, and nothing more handed on.
    void close() {
        {
            std::lock_guard lock(mutex_);
            over_ = true;
        }
        if (session_) {
            ACameraCaptureSession_stopRepeating(session_);
            ACameraCaptureSession_close(session_);
            session_ = nullptr;
        }
        if (device_) {
            ACameraDevice_close(device_);
            device_ = nullptr;
        }
        if (request_) {
            ACaptureRequest_free(request_);
            request_ = nullptr;
        }
        if (target_) {
            ACameraOutputTarget_free(target_);
            target_ = nullptr;
        }
        if (outputs_) {
            if (output_) ACaptureSessionOutputContainer_remove(outputs_, output_);
            ACaptureSessionOutputContainer_free(outputs_);
            outputs_ = nullptr;
        }
        if (output_) {
            ACaptureSessionOutput_free(output_);
            output_ = nullptr;
        }
        if (reader_) {
            AImageReader_setImageListener(reader_, nullptr);
            AImageReader_delete(reader_);
            reader_ = nullptr;
        }
    }

private:
    /// On the reader's thread: the newest picture, turned upright.
    void take(AImageReader* reader) {
        AImage* acquired = nullptr;
        if (AImageReader_acquireLatestImage(reader, &acquired) != AMEDIA_OK || !acquired) return;
        const std::unique_ptr<AImage, decltype(&AImage_delete)> image(acquired, AImage_delete);
        const Timestamp now = clock_.now();
        if (!rotation_checked_ || now - *rotation_checked_ >= kRotationEvery) {
            rotation_checked_ = now;
            display_ = display_rotation();
        }
        std::lock_guard lock(mutex_);
        if (over_ || !pacer_.admit(now)) return;
        VideoFrame picture = turned(image.get(), captured(image.get(), now));
        if (!picture.empty()) sink_(std::move(picture));
    }

    /// The degrees that turn a picture upright on the display as it is:
    /// against the display's rotation for a back camera, with it for a front
    /// one, whose pictures are seen as from its other side. An external
    /// camera does not turn with the device.
    int degrees() const {
        switch (camera_.facing) {
            case ACAMERA_LENS_FACING_BACK: return (camera_.orientation - display_ + 360) % 360;
            case ACAMERA_LENS_FACING_FRONT: return (camera_.orientation + display_) % 360;
            default: return camera_.orientation % 360;
        }
    }

    VideoFrame turned(AImage* image, Timestamp capture_time) {
        AImageCropRect crop{};
        if (AImage_getCropRect(image, &crop) != AMEDIA_OK) return {};
        Yuv420Planes planes;
        planes.width = uint32_t(std::max(crop.right - crop.left, 0)) & ~1u;
        planes.height = uint32_t(std::max(crop.bottom - crop.top, 0)) & ~1u;
        if (planes.width == 0 || planes.height == 0) return {};
        Yuv420Planes::Plane* each[3] = {&planes.y, &planes.u, &planes.v};
        for (int i = 0; i < 3; ++i) {
            uint8_t* data = nullptr;
            int length = 0;
            int32_t row_step = 0;
            int32_t pixel_step = 0;
            if (AImage_getPlaneData(image, i, &data, &length) != AMEDIA_OK ||
                AImage_getPlaneRowStride(image, i, &row_step) != AMEDIA_OK ||
                AImage_getPlanePixelStride(image, i, &pixel_step) != AMEDIA_OK || !data ||
                row_step <= 0 || pixel_step <= 0) {
                return {};
            }
            // The crop's first sample, and the last the picture reaches, inside.
            const int32_t shift = i == 0 ? 0 : 1;
            const int64_t first =
                int64_t(crop.top >> shift) * row_step + int64_t(crop.left >> shift) * pixel_step;
            const int64_t last = first + int64_t((planes.height >> shift) - 1) * row_step +
                                 int64_t((planes.width >> shift) - 1) * pixel_step;
            if (first < 0 || last >= length) return {};
            *each[i] = {data + first, row_step, pixel_step};
        }
        const int turn = degrees();
        MutableVideoFrame picture = MutableVideoFrame::allocate(
            options_.format, turned_width(planes.width, planes.height, turn),
            turned_height(planes.width, planes.height, turn));
        copy_turned(planes, turn, picture);
        return std::move(picture).freeze(capture_time, {ColorMatrix::BT601, ColorRange::Full});
    }

    /// When `image` was captured, by its stamp where that is boot time.
    Timestamp captured(AImage* image, Timestamp now) const {
        int64_t stamp = 0;
        if (!camera_.boot_time || AImage_getTimestamp(image, &stamp) != AMEDIA_OK) return now;
        const int64_t age = boot_time_now() - stamp;
        if (age < 0 || age > 1'000'000'000) return now;
        return now - std::chrono::duration_cast<Duration>(std::chrono::nanoseconds(age));
    }

    /// The end, said once; from the camera's threads.
    void end(Result<void> outcome) {
        std::lock_guard lock(mutex_);
        if (over_) return;
        over_ = true;
        if (ended_) ended_(std::move(outcome));
    }

    static void on_image(void* self, AImageReader* reader) {
        static_cast<CameraStream*>(self)->take(reader);
    }
    static void on_disconnected(void* self, ACameraDevice*) {
        LOG_INFO("engine", "the camera was taken away");
        static_cast<CameraStream*>(self)->end({});
    }
    static void on_error(void* self, ACameraDevice*, int error) {
        LOG_WARN("engine", "the camera failed: error " << error);
        static_cast<CameraStream*>(self)->end(fail(Errc::Internal));
    }
    static void on_session(void*, ACameraCaptureSession*) {}

    const Camera camera_;
    const CameraOptions options_;
    Clock& clock_;
    const VideoSource::Sink sink_;
    const VideoSource::EndHandler ended_;

    AImageReader_ImageListener image_listener_{this, &CameraStream::on_image};
    // Newer NDKs' structs have more members, left null.
    ACameraDevice_StateCallbacks device_callbacks_ = [this] {
        ACameraDevice_StateCallbacks callbacks{};
        callbacks.context = this;
        callbacks.onDisconnected = &CameraStream::on_disconnected;
        callbacks.onError = &CameraStream::on_error;
        return callbacks;
    }();
    ACameraCaptureSession_stateCallbacks session_callbacks_ = [this] {
        ACameraCaptureSession_stateCallbacks callbacks{};
        callbacks.context = this;
        callbacks.onClosed = &CameraStream::on_session;
        callbacks.onReady = &CameraStream::on_session;
        callbacks.onActive = &CameraStream::on_session;
        return callbacks;
    }();

    AImageReader* reader_ = nullptr;
    ACameraDevice* device_ = nullptr;
    ACaptureRequest* request_ = nullptr;
    ACameraOutputTarget* target_ = nullptr;
    ACaptureSessionOutputContainer* outputs_ = nullptr;
    ACaptureSessionOutput* output_ = nullptr;
    ACameraCaptureSession* session_ = nullptr;

    // The reader's thread's.
    std::optional<Timestamp> rotation_checked_;
    int display_ = 0;

    std::mutex mutex_;  ///< over the sink, the end and closing
    FramePacer pacer_;
    bool over_ = false;
};

class AndroidCamera final : public VideoSource {
public:
    AndroidCamera(const CameraOptions& options, Clock& clock) : options_(options), clock_(clock) {}
    ~AndroidCamera() override { stop(); }

    Result<void> start(Sink sink, EndHandler ended) override {
        std::lock_guard lock(mutex_);
        if (stream_ || missing_.joinable()) return fail(Errc::InvalidArgument);
        manager_.reset(ACameraManager_create());
        if (!manager_) return fail(Errc::Unavailable);
        std::vector<Camera> cameras = cameras_of(manager_.get());
        const auto it = std::ranges::find_if(
            cameras, [&](const Camera& camera) { return node_of(camera.id) == options_.node; });
        if (it == cameras.end()) {
            LOG_WARN("engine", "camera " << options_.node << " is not there");
            missing_ = std::thread([ended = std::move(ended)] {
                if (ended) ended(fail(Errc::Unavailable));
            });
            return {};
        }
        auto stream = std::make_unique<CameraStream>(std::move(*it), options_, clock_,
                                                     std::move(sink), std::move(ended));
        if (Result<void> started = stream->start(manager_.get()); !started) return started;
        stream_ = std::move(stream);
        return {};
    }

    void stop() override {
        std::lock_guard lock(mutex_);
        stream_.reset();
        manager_.reset();
        if (missing_.joinable()) missing_.join();
    }

private:
    const CameraOptions options_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    Manager manager_;
    std::unique_ptr<CameraStream> stream_;
    std::thread missing_;  ///< saying a camera that is not there ended
};

}  // namespace
}  // namespace ratcast::android

namespace ratcast::backend {

Result<std::vector<CameraInfo>> list_cameras() {
    const android::Manager manager(ACameraManager_create());
    if (!manager) return fail(Errc::Unavailable);
    return android::infos_of(android::cameras_of(manager.get()));
}

Result<std::unique_ptr<VideoSource>> open_camera(const CameraOptions& options) {
    CameraOptions resolved = options;
    if (resolved.node == 0) {
        Result<std::vector<CameraInfo>> cameras = list_cameras();
        if (!cameras) return fail(cameras.error());
        if (cameras->empty()) return fail(Errc::Unavailable);
        resolved.node = cameras->front().node;
    }
    return std::unique_ptr<VideoSource>(
        std::make_unique<android::AndroidCamera>(resolved, SteadyClock::instance()));
}

}  // namespace ratcast::backend
