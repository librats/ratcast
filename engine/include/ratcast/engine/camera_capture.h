#pragma once

/**
 * @file camera_capture.h
 * @brief Pictures from a camera.
 *
 * The cameras are the system's video sources that say they are one — on
 * Linux PipeWire's nodes of a camera's role, which V4L2 and libcamera
 * devices both appear as; on Windows Media Foundation's video capture
 * devices, whose node is a hash of the device's path, the same from one
 * listing to the next. A camera sends pictures at its own pace in one of
 * its modes: the first it lists that is no larger than asked and runs at 15
 * pictures a second or more, else the first no larger, else its first. What
 * it sends — packed 4:2:2 (YUY2), 4:2:0, or RGB — comes converted into the
 * format asked for, in its own colour space (BT.601 unless it says another).
 * Compressed modes (MJPEG) are not taken.
 *
 * The capture ends by itself — the EndHandler hears it — when the camera
 * goes: unplugged, or taken away.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"
#include "ratcast/engine/video_source.h"

namespace ratcast {

struct CameraInfo {
    uint32_t node = 0;  ///< what CameraOptions::node takes
    std::string name;   ///< as the system names it, for a UI
};

struct CameraOptions {
    /// Which camera: one list_cameras() gave, or 0 for the first it gives.
    uint32_t node = 0;
    PixelFormat format = PixelFormat::NV12;  ///< 4:2:0
    /// The largest picture to take, and the most pictures a second.
    uint32_t max_width = 1280;
    uint32_t max_height = 720;
    uint32_t max_frame_rate = 30;
};

/// The cameras there are now. Fails with Unavailable where this system has no
/// way to reach them; none there is an empty list.
Result<std::vector<CameraInfo>> list_cameras();

/// A capture of a camera, ready to start. Fails with InvalidArgument for a
/// format other than NV12 or I420, a size or a rate of 0; Unavailable when
/// there is no camera (with node 0) or no way to reach one. A node that is
/// not there ends the capture as it starts.
Result<std::unique_ptr<VideoSource>> open_camera(const CameraOptions& options = {});

}  // namespace ratcast
