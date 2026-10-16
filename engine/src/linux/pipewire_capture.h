#pragma once

/**
 * @file pipewire_capture.h
 * @brief Pictures from a PipeWire video node.
 *
 * The node is a screen or window that the portal handed out, over the
 * connection the portal opened, or in tests any video source on the user's
 * own PipeWire daemon. Pictures arrive in shared memory as BGRx — the
 * compositor has already copied them out of GPU memory — and are converted
 * straight into the format asked for: one pass, after which the buffer goes
 * back to the compositor at once.
 *
 * The cursor is whatever the node draws into its pictures; the portal asks the
 * compositor to draw it.
 */

#include <cstdint>
#include <memory>
#include <mutex>

#include "ratcast/core/time.h"
#include "ratcast/engine/video_frame.h"
#include "ratcast/engine/video_source.h"

namespace ratcast::pipewire {

struct CaptureConfig {
    /// A connection to PipeWire (the portal's OpenPipeWireRemote), or -1 for
    /// the user's daemon. Duplicated, not taken over.
    int fd = -1;
    uint32_t node = 0;  ///< the node to capture
    PixelFormat format = PixelFormat::NV12;
    ColorSpace color_space;
    uint32_t max_frame_rate = 60;  ///< 0 for as many as the node sends
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
