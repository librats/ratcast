#include "ratcast/engine/camera_capture.h"

#include "capture/camera_backend.h"

#include <algorithm>

#include "librats/util/logger.h"

namespace ratcast {

Result<std::vector<CameraInfo>> list_cameras() { return backend::list_cameras(); }

Result<std::unique_ptr<VideoSource>> open_camera(const CameraOptions& options) {
    const bool yuv_420 = options.format == PixelFormat::NV12 || options.format == PixelFormat::I420;
    if (!yuv_420 || options.max_width == 0 || options.max_height == 0 ||
        options.max_frame_rate == 0) {
        return fail(Errc::InvalidArgument);
    }
    if (options.node != 0 || options.id.empty()) return backend::open_camera(options);
    // By its id: whichever node it has now.
    Result<std::vector<CameraInfo>> cameras = backend::list_cameras();
    if (!cameras) return fail(cameras.error());
    CameraOptions resolved = options;
    if (const auto it = std::ranges::find(*cameras, options.id, &CameraInfo::id);
        it != cameras->end()) {
        resolved.node = it->node;
    } else {
        LOG_INFO("engine", "camera " << options.id << " is not here: the first there is");
    }
    return backend::open_camera(resolved);
}

}  // namespace ratcast
