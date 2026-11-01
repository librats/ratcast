#pragma once

/**
 * @file surface_drawer.h
 * @brief Pictures drawn into a window through OpenGL ES: what MediaCodec's
 * encoder reads through its input surface.
 *
 * An EGL context of its own, recordable (what an encoder's surface wants), on
 * the window; each picture drawn over the whole window, then handed to its
 * consumer with its time. A picture in an AHardwareBuffer (hardware_picture.h)
 * is read where it is, through an EGLImage of it; one in memory, NV12 or
 * I420, goes up as textures of its planes and is turned into colour by its
 * own colour space. Either way the window's consumer is given RGB, which an
 * encoder turns into YUV as its format says.
 *
 * The context is current only while drawing, on the thread that draws, which
 * finds whatever it had current as it was: a drawer belongs to no thread.
 * The default display is the process's, Qt's too, and is never terminated.
 */

#include <android/native_window.h>

#include <cstdint>
#include <memory>

#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"

namespace ratcast::android {

class SurfaceDrawer {
public:
    /// A drawer on `window`, which it holds. Fails with Unavailable where
    /// EGL or OpenGL ES 3 will not draw into it.
    static Result<std::unique_ptr<SurfaceDrawer>> open(ANativeWindow* window);

    virtual ~SurfaceDrawer() = default;

    /// `picture` — of hardware_buffer_of(), or NV12 or I420 in memory — drawn
    /// over the whole window and handed to its consumer, stamped `time_ns`.
    /// Fails with InvalidArgument for another picture, Internal for one not
    /// drawn.
    virtual Result<void> draw(const VideoFrame& picture, int64_t time_ns) = 0;
};

}  // namespace ratcast::android
