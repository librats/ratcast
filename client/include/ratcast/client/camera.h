#pragma once

/**
 * @file camera.h
 * @brief A camera in a call: published beside the rest, and seen by its own
 * side as it goes.
 *
 * publish_camera() puts a camera (open_camera()'s, or any source) on a call as
 * a video stream labelled kCameraLabel, which the other side plays apart from
 * the screen, in its own time. It is a small picture beside a big one, so it
 * is coded small: no more than 1 Mbit/s, rate control sharing the path with
 * the screen in proportion (the screen's 8 to its 1). with_preview() lets the
 * sender see what goes.
 */

#include <memory>

#include "ratcast/client/call.h"
#include "ratcast/client/video_publisher.h"
#include "ratcast/core/result.h"
#include "ratcast/engine/camera_capture.h"
#include "ratcast/engine/video_source.h"

namespace ratcast {

/// The stream a camera goes as, beside a screen's pictures (1) and sound (2)
/// and a voice (4).
inline constexpr StreamId kCameraStream = 3;

/// What a camera in a call is opened with: 640 x 480 at most, 30 a second.
CameraOptions call_camera_options();

/// Publish `camera` on `call` as this side's camera. Fails as Call::publish
/// does.
Result<VideoPublisher*> publish_camera(Call& call, std::unique_ptr<VideoSource> camera,
                                       StreamId stream = kCameraStream);

/// `source`, each of its pictures handed to `preview` as well (on the
/// source's thread, before the publisher has it). Pictures are shared, not
/// copied.
std::unique_ptr<VideoSource> with_preview(std::unique_ptr<VideoSource> source,
                                          VideoSource::Sink preview);

}  // namespace ratcast
