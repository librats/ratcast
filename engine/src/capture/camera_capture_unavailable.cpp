// Cameras where the engine has none yet (macOS until its backend lands;
// Linux built without PipeWire).

#include "capture/camera_backend.h"

namespace ratcast::backend {

Result<std::vector<CameraInfo>> list_cameras() { return fail(Errc::Unavailable); }

Result<std::unique_ptr<VideoSource>> open_camera(const CameraOptions&) {
    return fail(Errc::Unavailable);
}

}  // namespace ratcast::backend
