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
 */

#include <functional>

#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"

namespace ratcast {

class VideoSource {
public:
    using Sink = std::function<void(VideoFrame picture)>;

    virtual ~VideoSource() = default;

    /// Start delivering pictures to `sink`. Fails if the source cannot start
    /// (no permission, the window is gone) or has already started.
    virtual Result<void> start(Sink sink) = 0;

    /// Stop. When this returns, the sink is not running and will not be called
    /// again.
    virtual void stop() = 0;
};

}  // namespace ratcast
