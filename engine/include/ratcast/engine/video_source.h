#pragma once

/**
 * @file video_source.h
 * @brief Where pictures come from: a screen, a window, a camera, a file.
 *
 * A source pushes pictures as it gets them, on a thread of its own. The sink
 * must not block — capture APIs hand out a few buffers and stop when they are
 * not returned — so it should pass the picture to whatever encodes it and
 * return. A source that outruns its sink is the sink's to handle: drop the
 * older picture, never queue a backlog.
 *
 * Pictures in 4:2:0 formats have even sizes (an odd last row or column is
 * left out), which is what encoders take.
 */

#include <functional>

#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"

namespace ratcast {

class VideoSource {
public:
    using Sink = std::function<void(VideoFrame picture)>;

    /// How a source that stops by itself says so: once, from its thread, with
    /// success when there is nothing more to show (the file ended, the window
    /// closed, the user stopped sharing) and the failure otherwise.
    using EndHandler = std::function<void(Result<void> outcome)>;

    virtual ~VideoSource() = default;

    /// Start delivering pictures to `sink`; `ended` (may be empty) hears when
    /// the source stops by itself. Neither may call stop(). Fails if the
    /// source cannot start (no permission, the window is gone) or has already
    /// started.
    virtual Result<void> start(Sink sink, EndHandler ended) = 0;

    /// Stop. When this returns, neither handler is running or will be called
    /// again. The source may be started again.
    virtual void stop() = 0;
};

}  // namespace ratcast
