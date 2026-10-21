#include "ratcast/client/camera.h"

#include <utility>

namespace ratcast {
namespace {

class Previewed final : public VideoSource {
public:
    Previewed(std::unique_ptr<VideoSource> source, Sink preview)
        : source_(std::move(source)), preview_(std::move(preview)) {}
    ~Previewed() override { stop(); }

    Result<void> start(Sink sink, EndHandler ended) override {
        return source_->start(
            [this, sink = std::move(sink)](VideoFrame picture) {
                if (preview_) preview_(picture);
                sink(std::move(picture));
            },
            std::move(ended));
    }
    void stop() override { source_->stop(); }

private:
    std::unique_ptr<VideoSource> source_;
    const Sink preview_;
};

}  // namespace

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

std::unique_ptr<VideoSource> with_preview(std::unique_ptr<VideoSource> source,
                                          VideoSource::Sink preview) {
    return std::make_unique<Previewed>(std::move(source), std::move(preview));
}

}  // namespace ratcast
