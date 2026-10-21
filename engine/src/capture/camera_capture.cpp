#include "ratcast/engine/camera_capture.h"

#include "capture/camera_backend.h"

namespace ratcast {

Result<std::vector<CameraInfo>> list_cameras() { return backend::list_cameras(); }

Result<std::unique_ptr<VideoSource>> open_camera(const CameraOptions& options) {
    const bool yuv_420 = options.format == PixelFormat::NV12 || options.format == PixelFormat::I420;
    if (!yuv_420 || options.max_width == 0 || options.max_height == 0 ||
        options.max_frame_rate == 0) {
        return fail(Errc::InvalidArgument);
    }
    return backend::open_camera(options);
}

}  // namespace ratcast
