#pragma once

/**
 * @file cursor_source.h
 * @brief The pointer apart from the screen's pictures: its image and where it is.
 *
 * A screen captured without its pointer (ScreenCaptureOptions::cursor off) and
 * the pointer sent beside it lets the other side draw it sharp, at its own
 * rate, and move it at once with its own mouse — what remote control wants,
 * and a full-screen share where the pictures come scaled.
 *
 * A source looks at the pointer on a thread of its own and tells what
 * changed: an image the first time it is seen (its id comes back whenever
 * the pointer looks so again, for as long as the source lives), then where
 * the pointer is, whether it shows and which image it wears. Each state names
 * an image told before it, or none (0) when the pointer's image could not be
 * read — draw a plain arrow then.
 *
 * Windows: the desktop's pointer (GetCursorInfo), its images read from the
 * icon's bitmaps (monochrome and inverting ones made black with a white edge;
 * an animated one by its first frame); positions in the desktop's physical
 * pixels, as the screens' rectangles are for a per-monitor DPI aware process.
 * Elsewhere Unavailable for now.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "ratcast/core/result.h"

namespace ratcast {

/// The largest image a source tells; a larger pointer goes with image 0.
inline constexpr uint32_t kMaxCursorSide = 256;

struct CursorImage {
    uint32_t id = 0;  ///< never 0
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t hot_x = 0;  ///< the point that points, from the top left
    uint32_t hot_y = 0;
    std::vector<uint8_t> rgba;  ///< width * height * 4, straight alpha, rows top first
};

struct CursorState {
    int32_t x = 0;  ///< the hotspot on the desktop, in its pixels
    int32_t y = 0;
    bool visible = true;  ///< false where an application hid it, or it cannot be seen
    uint32_t image = 0;   ///< a CursorImage's id, 0 for none
    friend bool operator==(const CursorState&, const CursorState&) = default;
};

struct CursorSourceOptions {
    uint32_t rate = 120;  ///< looks a second
};

class CursorSource {
public:
    using ImageHandler = std::function<void(const CursorImage& image)>;
    using StateHandler = std::function<void(const CursorState& state)>;

    virtual ~CursorSource() = default;

    /// Start telling the pointer's images and its state, from the source's
    /// thread: the state at once, then each time it changes. Neither handler
    /// may call stop(). Fails if already started.
    virtual Result<void> start(ImageHandler image, StateHandler state) = 0;

    /// Stop. When this returns no handler is running or will be called again.
    /// The source may be started again, and tells its images afresh.
    virtual void stop() = 0;
};

/// The pointer of the desktop this process runs on. Unavailable where the
/// system's pointer cannot be read apart from its pictures.
Result<std::unique_ptr<CursorSource>> open_cursor_source(const CursorSourceOptions& options = {});

}  // namespace ratcast
