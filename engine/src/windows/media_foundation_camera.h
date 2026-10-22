#pragma once

/**
 * @file media_foundation_camera.h
 * @brief Cameras through Media Foundation.
 *
 * The cameras are Media Foundation's video capture devices, each known by
 * its symbolic link, which a node is a hash of: the same camera has the same
 * node from one listing to the next, while it stays plugged into the same
 * port. A camera lists its modes; the one taken is the first no larger than
 * asked at 15 pictures a second or more, else the first no larger, else its
 * first — of the modes in a format read here (YUY2, NV12, I420, RGB32), as
 * compressed ones (MJPEG, H.264) are not taken. Its pictures come converted
 * into the format asked for, in its own colour space (BT.601 unless it says
 * another).
 *
 * Pictures arrive on Media Foundation's work queue, read one after another
 * by an asynchronous source reader; stop() flushes it and waits for it.
 */

#include <mfobjects.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ratcast/core/time.h"
#include "ratcast/engine/video_frame.h"

namespace ratcast::mf {

/// A mode a camera sends pictures in, as it lists it.
struct CameraMode {
    PixelFormat format = PixelFormat::YUY2;  ///< BGRA for RGB32
    uint32_t width = 0;
    uint32_t height = 0;
    double frame_rate = 0;  ///< pictures a second, at most
    /// Bytes from one row to the next, of the first plane; negative for RGB
    /// stored bottom row first; 0 when the camera does not say.
    int32_t stride = 0;
    ColorSpace color_space{ColorMatrix::BT601, ColorRange::Limited};
    uint32_t index = 0;  ///< among the camera's native media types
};

/// The node of the camera whose symbolic link is `link`: never 0.
uint32_t node_of(std::wstring_view link);

/// Which of `modes` to take, listed in the camera's order; nullopt for none.
std::optional<size_t> choose_mode(std::span<const CameraMode> modes, uint32_t max_width,
                                  uint32_t max_height);

/// The picture in `buffer`, a sample's of a camera in `mode`, in `format`
/// (NV12 or I420), stamped `captured`; nullopt for a buffer too small for it.
std::optional<VideoFrame> read_picture(IMFMediaBuffer& buffer, const CameraMode& mode,
                                       PixelFormat format, Timestamp captured);

}  // namespace ratcast::mf
