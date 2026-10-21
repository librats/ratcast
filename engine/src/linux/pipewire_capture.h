#pragma once

/**
 * @file pipewire_capture.h
 * @brief Pictures from a PipeWire video node.
 *
 * The node is a screen or window that the portal handed out, over the
 * connection the portal opened, a camera on the user's own daemon, or in tests
 * any video source there. Pictures arrive in shared memory — a screen's as
 * BGRx, the compositor having copied them out of GPU memory; a camera's as
 * YUY2, NV12, I420 or BGRx, in one block or a block a plane — and are
 * converted straight into the format asked for: one pass, after which the
 * buffer goes back at once.
 *
 * A screen sends pictures as it changes, of any size, cropped as its metadata
 * says; the cursor is whatever the node draws into them (the portal asks the
 * compositor to draw it). A camera sends them at its own pace in one of its
 * modes, the first of them that suits: no larger than `max_size` and 15
 * pictures a second or more, else no larger, else any.
 */

#include <cstdint>
#include <memory>
#include <mutex>

#include "ratcast/core/time.h"
#include "ratcast/engine/video_frame.h"
#include "ratcast/engine/video_source.h"

namespace ratcast::pipewire {

struct CaptureConfig {
    enum class Kind : uint8_t { Screen, Camera };

    Kind kind = Kind::Screen;
    /// A connection to PipeWire (the portal's OpenPipeWireRemote), or -1 for
    /// the user's daemon. Duplicated, not taken over.
    int fd = -1;
    uint32_t node = 0;  ///< the node to capture
    PixelFormat format = PixelFormat::NV12;
    ColorSpace color_space;
    uint32_t max_frame_rate = 60;  ///< 0 for as many as the node sends
    /// A camera's largest picture.
    uint32_t max_width = 1280;
    uint32_t max_height = 720;
};

class Capture final : public VideoSource {
public:
    explicit Capture(CaptureConfig config, Clock& clock = SteadyClock::instance());
    ~Capture() override;

    /// Fails with Unavailable when PipeWire cannot be reached. A node that
    /// does not exist, or will not stream, ends the capture with a failure.
    Result<void> start(Sink sink, EndHandler ended) override;
    void stop() override;

private:
    class Stream;  // the PipeWire side, one per start()

    const CaptureConfig config_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    std::unique_ptr<Stream> stream_;
};

}  // namespace ratcast::pipewire
