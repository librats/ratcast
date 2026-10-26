// Cameras on Linux: PipeWire's video sources of a camera's role, read on the
// user's own daemon (a sandbox would go through the camera portal instead).

#include "capture/camera_backend.h"

#include "librats/util/logger.h"
#include "linux/pipewire_capture.h"
#include "linux/pipewire_registry.h"

namespace ratcast::backend {

Result<std::vector<CameraInfo>> list_cameras() {
    Result<std::vector<pipewire::NodeInfo>> nodes = pipewire::list_nodes();
    if (!nodes) return fail(nodes.error());
    std::vector<CameraInfo> cameras;
    for (pipewire::NodeInfo& node : *nodes) {
        if (node.media_class != "Video/Source" || node.role != "Camera") continue;
        std::string name = !node.description.empty() ? std::move(node.description)
                           : !node.name.empty()      ? node.name
                                                     : std::string("Camera");
        cameras.push_back({.node = node.id, .id = std::move(node.name), .name = std::move(name)});
    }
    return cameras;
}

Result<std::unique_ptr<VideoSource>> open_camera(const CameraOptions& options) {
    uint32_t node = options.node;
    if (node == 0) {
        Result<std::vector<CameraInfo>> cameras = list_cameras();
        if (!cameras) return fail(cameras.error());
        if (cameras->empty()) {
            LOG_INFO("engine", "no camera here");
            return fail(Errc::Unavailable);
        }
        node = cameras->front().node;
    }
    pipewire::CaptureConfig config;
    config.kind = pipewire::CaptureConfig::Kind::Camera;
    config.node = node;
    config.format = options.format;
    config.max_frame_rate = options.max_frame_rate;
    config.max_width = options.max_width;
    config.max_height = options.max_height;
    return std::unique_ptr<VideoSource>(std::make_unique<pipewire::Capture>(config));
}

}  // namespace ratcast::backend
