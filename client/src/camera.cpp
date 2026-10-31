#include "ratcast/client/camera.h"

#include <utility>

namespace ratcast {

CameraOptions call_camera_options() {
    return CameraOptions{.max_width = 640, .max_height = 480, .max_frame_rate = 30};
}

Result<VideoPublisher*> publish_camera(Call& call, std::unique_ptr<VideoSource> camera,
                                       StreamId stream) {
    VideoPublisherConfig config;
    config.stream = stream;
    config.label = std::string(kCameraLabel);
    config.encoder.frame_rate_num = 30;
    config.encoder.bitrate = 1'000'000;
    config.start_bitrate = 300'000;
    config.min_bitrate = 100'000;
    return call.publish(std::move(camera), config);
}

}  // namespace ratcast
