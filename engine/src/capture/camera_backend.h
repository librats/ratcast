#pragma once

/**
 * @file camera_backend.h
 * @brief What a camera backend provides behind camera_capture.h.
 *
 * The public entry points check their arguments and hand valid requests here,
 * so every backend — PipeWire, or none at all — answers the same way to the
 * same mistakes.
 */

#include <memory>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/engine/camera_capture.h"

namespace ratcast::backend {

Result<std::vector<CameraInfo>> list_cameras();

/// Precondition: `options` passed open_camera()'s checks.
Result<std::unique_ptr<VideoSource>> open_camera(const CameraOptions& options);

}  // namespace ratcast::backend
