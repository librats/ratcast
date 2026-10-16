// Screen capture on Linux: the portal grants a PipeWire node, which
// pipewire::Capture then reads. The cast lives as long as the capture does.

#include "ratcast/engine/screen_capture.h"

#include "linux/pipewire_capture.h"
#include "linux/portal.h"

namespace ratcast {
namespace {

class LinuxScreenCapture final : public ScreenCapture {
public:
    LinuxScreenCapture(std::unique_ptr<portal::ScreenCast> cast,
                       const ScreenCaptureOptions& options)
        : cast_(std::move(cast)), capture_(config(*cast_, options)) {}

    Result<void> start(Sink sink, EndHandler ended) override {
        return capture_.start(std::move(sink), std::move(ended));
    }
    void stop() override { capture_.stop(); }
    const std::string& restore_token() const noexcept override { return cast_->restore_token(); }

private:
    static pipewire::CaptureConfig config(const portal::ScreenCast& cast,
                                          const ScreenCaptureOptions& options) {
        pipewire::CaptureConfig config;
        config.fd = cast.pipewire_fd();
        config.node = cast.node();
        config.format = options.format;
        config.color_space = options.color_space;
        config.max_frame_rate = options.max_frame_rate;
        return config;
    }

    // Declared in this order so that the capture stops before the cast ends.
    std::unique_ptr<portal::ScreenCast> cast_;
    pipewire::Capture capture_;
};

}  // namespace

Result<std::unique_ptr<ScreenCapture>> open_screen_capture(const ScreenCaptureOptions& options,
                                                           std::stop_token stop) {
    portal::ScreenCastRequest request;
    request.monitors = options.monitors;
    request.windows = options.windows;
    request.cursor = options.cursor;
    request.restore_token = options.restore_token;
    Result<std::unique_ptr<portal::ScreenCast>> cast =
        portal::start_screen_cast(request, std::move(stop));
    if (!cast) return fail(cast.error());
    return std::unique_ptr<ScreenCapture>(
        std::make_unique<LinuxScreenCapture>(std::move(*cast), options));
}

}  // namespace ratcast
