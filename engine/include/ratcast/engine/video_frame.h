#pragma once

/**
 * @file video_frame.h
 * @brief A raw picture: what sources produce, encoders take and decoders give.
 *
 * A VideoFrame is a value, cheap to copy: it shares the memory it describes
 * and never copies it. That memory has one of two owners:
 *
 *   - a pooled Buffer, for frames the engine allocates (MutableVideoFrame);
 *   - anything else, held by a shared_ptr — a decoder's own frame, a capture
 *     buffer — so pictures made elsewhere are passed on without a copy.
 *
 * Planes allocated here start and have strides on 64-byte boundaries, which is
 * what SIMD code in encoders and converters wants. Pictures in GPU memory will
 * be a third kind of frame; the accessors here describe CPU-visible planes.
 *
 * A YUV picture also says how its numbers map to colour (ColorSpace), which an
 * encoder signals in the stream so that the far side shows the same colours.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "ratcast/core/buffer.h"
#include "ratcast/core/time.h"

namespace ratcast {

enum class PixelFormat : uint8_t {
    I420,  ///< 8-bit 4:2:0 in three planes: Y, then U and V at half size
    NV12,  ///< 8-bit 4:2:0 in two planes: Y, then U and V interleaved at half size
    BGRA,  ///< 8-bit packed blue, green, red, alpha (alpha may be undefined)
    /// 8-bit 4:2:2 packed, Y0 U Y1 V for each two pixels: what cameras give.
    /// Read only, into 4:2:0; no encoder takes it.
    YUY2,
};

/// The matrix that turns RGB into YUV. Primaries and transfer are always
/// BT.709's, which sRGB shares: what screens show and cameras give.
enum class ColorMatrix : uint8_t {
    BT709,  ///< HD and up
    BT601,  ///< SD
};

enum class ColorRange : uint8_t {
    Limited,  ///< luma 16-235, chroma 16-240: what video uses
    Full,     ///< 0-255
};

/// How a YUV picture's values map to colour. RGB pictures are sRGB at full
/// range; for them the colour space means nothing.
struct ColorSpace {
    ColorMatrix matrix = ColorMatrix::BT709;
    ColorRange range = ColorRange::Limited;

    friend bool operator==(const ColorSpace&, const ColorSpace&) = default;
};

/// How many planes a picture in `format` has.
constexpr size_t plane_count(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::I420: return 3;
        case PixelFormat::NV12: return 2;
        case PixelFormat::BGRA:
        case PixelFormat::YUY2: return 1;
    }
    return 0;
}

/// Bytes of one row of plane `plane` of a `width`-pixel picture, unpadded.
size_t plane_row_bytes(PixelFormat format, size_t plane, uint32_t width) noexcept;

/// Rows of plane `plane` of a `height`-pixel picture.
size_t plane_rows(PixelFormat format, size_t plane, uint32_t height) noexcept;

class VideoFrame {
public:
    static constexpr size_t kMaxPlanes = 3;

    struct Plane {
        const uint8_t* data = nullptr;
        size_t stride = 0;  ///< bytes from one row to the next
    };

    /// An empty frame: no picture.
    VideoFrame() noexcept = default;

    /// A frame over planes someone else owns; `owner` keeps them alive as long
    /// as any copy of the frame exists. Without an owner the frame is a view,
    /// valid only as long as the planes are.
    VideoFrame(PixelFormat format, uint32_t width, uint32_t height, std::span<const Plane> planes,
               std::shared_ptr<const void> owner, Timestamp capture_time,
               ColorSpace color_space = {}) noexcept;

    bool empty() const noexcept { return width_ == 0; }
    PixelFormat format() const noexcept { return format_; }
    uint32_t width() const noexcept { return width_; }
    uint32_t height() const noexcept { return height_; }
    size_t plane_count() const noexcept { return ratcast::plane_count(format_); }
    const Plane& plane(size_t i) const noexcept { return planes_[i]; }

    /// When the picture was captured, on the local monotonic clock.
    Timestamp capture_time() const noexcept { return capture_time_; }
    void set_capture_time(Timestamp time) noexcept { capture_time_ = time; }

    ColorSpace color_space() const noexcept { return color_space_; }
    void set_color_space(ColorSpace color_space) noexcept { color_space_ = color_space; }

private:
    friend class MutableVideoFrame;

    PixelFormat format_ = PixelFormat::I420;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::array<Plane, kMaxPlanes> planes_{};
    Timestamp capture_time_{};
    ColorSpace color_space_;
    Buffer storage_;                     ///< owner of planes allocated here
    std::shared_ptr<const void> owner_;  ///< owner of planes from elsewhere
};

/// A frame being drawn: writable planes in pooled memory, frozen into a
/// VideoFrame once complete. Move-only.
class MutableVideoFrame {
public:
    struct Plane {
        uint8_t* data = nullptr;
        size_t stride = 0;
    };

    /// A `width` x `height` picture in `format`, contents unspecified.
    /// Precondition: width and height > 0.
    static MutableVideoFrame allocate(PixelFormat format, uint32_t width, uint32_t height);
    static MutableVideoFrame allocate(PixelFormat format, uint32_t width, uint32_t height,
                                      BufferPool& pool);

    MutableVideoFrame(MutableVideoFrame&&) noexcept = default;
    MutableVideoFrame& operator=(MutableVideoFrame&&) noexcept = default;

    PixelFormat format() const noexcept { return format_; }
    uint32_t width() const noexcept { return width_; }
    uint32_t height() const noexcept { return height_; }
    const Plane& plane(size_t i) const noexcept { return planes_[i]; }

    /// The finished picture, captured at `capture_time`. Leaves this empty.
    VideoFrame freeze(Timestamp capture_time, ColorSpace color_space = {}) && noexcept;

private:
    MutableVideoFrame() = default;

    PixelFormat format_ = PixelFormat::I420;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::array<Plane, VideoFrame::kMaxPlanes> planes_{};
    MutableBuffer storage_;
};

}  // namespace ratcast
