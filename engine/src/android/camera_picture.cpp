// A camera's picture turned upright: see android/camera_picture.h.

#include "android/camera_picture.h"

#include <cstddef>
#include <cstring>

namespace ratcast::android {
namespace {

/// Where row `row` of a plane turned by `degrees` starts in `from`, and the
/// step from one of its samples to the next: the turned picture's rows walk
/// the source's columns or rows, forwards or back. `width` x `height` is
/// the plane's size before turning.
struct Walk {
    const uint8_t* start;
    ptrdiff_t step;
};

Walk walk(const Yuv420Planes::Plane& from, uint32_t width, uint32_t height, int degrees,
          uint32_t row) {
    const auto at = [&](uint32_t x, uint32_t y) {
        return from.data + ptrdiff_t(y) * from.row_step + ptrdiff_t(x) * from.pixel_step;
    };
    switch (degrees) {
        case 90: return {at(row, height - 1), -ptrdiff_t(from.row_step)};
        case 180: return {at(width - 1, height - 1 - row), -ptrdiff_t(from.pixel_step)};
        case 270: return {at(width - 1 - row, 0), ptrdiff_t(from.row_step)};
        default: return {at(0, row), ptrdiff_t(from.pixel_step)};
    }
}

void copy_plane(const Yuv420Planes::Plane& from, uint32_t width, uint32_t height, int degrees,
                uint8_t* to, size_t stride) {
    const uint32_t rows = turned_height(width, height, degrees);
    const uint32_t columns = turned_width(width, height, degrees);
    for (uint32_t row = 0; row < rows; ++row) {
        const Walk line = walk(from, width, height, degrees, row);
        uint8_t* out = to + row * stride;
        if (line.step == 1) {
            std::memcpy(out, line.start, columns);
            continue;
        }
        for (uint32_t column = 0; column < columns; ++column)
            out[column] = line.start[column * line.step];
    }
}

}  // namespace

void copy_turned(const Yuv420Planes& from, int degrees, MutableVideoFrame& into) {
    const uint32_t width = from.width;
    const uint32_t height = from.height;
    copy_plane(from.y, width, height, degrees, into.plane(0).data, into.plane(0).stride);
    if (into.format() == PixelFormat::I420) {
        copy_plane(from.u, width / 2, height / 2, degrees, into.plane(1).data,
                   into.plane(1).stride);
        copy_plane(from.v, width / 2, height / 2, degrees, into.plane(2).data,
                   into.plane(2).stride);
        return;
    }
    // NV12: U and V interleaved, each walked as its own plane.
    const uint32_t rows = turned_height(width / 2, height / 2, degrees);
    const uint32_t columns = turned_width(width / 2, height / 2, degrees);
    for (uint32_t row = 0; row < rows; ++row) {
        const Walk u = walk(from.u, width / 2, height / 2, degrees, row);
        const Walk v = walk(from.v, width / 2, height / 2, degrees, row);
        uint8_t* out = into.plane(1).data + row * into.plane(1).stride;
        // A camera's own NV12, not turned: copied as it is.
        if (u.step == 2 && v.step == 2 && v.start == u.start + 1) {
            std::memcpy(out, u.start, size_t(columns) * 2);
            continue;
        }
        for (uint32_t column = 0; column < columns; ++column) {
            out[2 * column] = u.start[column * u.step];
            out[2 * column + 1] = v.start[column * v.step];
        }
    }
}

}  // namespace ratcast::android
