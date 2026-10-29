#pragma once

/**
 * @file camera_picture.h
 * @brief A camera's YUV 4:2:0 picture, turned upright into a frame.
 *
 * Android's cameras hand out YUV_420_888: three planes, each with its own
 * row and pixel steps (the chroma planes of one interleaved buffer, pixel
 * step 2, as most do; or apart, step 1), the picture as the sensor sees it.
 * Turned here, in one pass, by the angle that makes it upright, into NV12 or
 * I420.
 */

#include <cstdint>

#include "ratcast/engine/video_frame.h"

namespace ratcast::android {

/// A 4:2:0 picture in planes of any steps, `width` x `height` (even).
struct Yuv420Planes {
    struct Plane {
        const uint8_t* data = nullptr;  ///< the picture's first sample
        int32_t row_step = 0;           ///< bytes from one row to the next
        int32_t pixel_step = 1;         ///< bytes from one sample to the next
    };
    Plane y, u, v;
    uint32_t width = 0;
    uint32_t height = 0;
};

/// The size of `width` x `height` turned by `degrees`.
constexpr uint32_t turned_width(uint32_t width, uint32_t height, int degrees) noexcept {
    return degrees % 180 == 0 ? width : height;
}
constexpr uint32_t turned_height(uint32_t width, uint32_t height, int degrees) noexcept {
    return degrees % 180 == 0 ? height : width;
}

/// `from` turned clockwise by `degrees` (0, 90, 180 or 270) into `into`, NV12
/// or I420 of the turned size. Precondition: so.
void copy_turned(const Yuv420Planes& from, int degrees, MutableVideoFrame& into);

}  // namespace ratcast::android
