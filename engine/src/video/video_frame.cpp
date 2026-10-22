#include "ratcast/engine/video_frame.h"

#include <cassert>
#include <cstdint>
#include <utility>

namespace ratcast {
namespace {

constexpr size_t kAlignment = 64;

constexpr size_t align_up(size_t value) noexcept {
    return (value + kAlignment - 1) / kAlignment * kAlignment;
}

}  // namespace

size_t plane_row_bytes(PixelFormat format, size_t plane, uint32_t width) noexcept {
    const size_t half = (size_t{width} + 1) / 2;
    switch (format) {
        case PixelFormat::I420: return plane == 0 ? width : half;
        case PixelFormat::NV12: return plane == 0 ? width : half * 2;
        case PixelFormat::BGRA: return size_t{width} * 4;
        case PixelFormat::YUY2: return half * 4;
    }
    return 0;
}

size_t plane_rows(PixelFormat format, size_t plane, uint32_t height) noexcept {
    switch (format) {
        case PixelFormat::I420:
        case PixelFormat::NV12: return plane == 0 ? height : (size_t{height} + 1) / 2;
        case PixelFormat::BGRA:
        case PixelFormat::YUY2: return height;
    }
    return 0;
}

VideoFrame::VideoFrame(PixelFormat format, uint32_t width, uint32_t height,
                       std::span<const Plane> planes, std::shared_ptr<const void> owner,
                       Timestamp capture_time, ColorSpace color_space) noexcept
    : format_(format),
      width_(width),
      height_(height),
      capture_time_(capture_time),
      color_space_(color_space),
      owner_(std::move(owner)) {
    assert(planes.size() == ratcast::plane_count(format));
    for (size_t i = 0; i < planes.size(); ++i) planes_[i] = planes[i];
}

VideoFrame::VideoFrame(PixelFormat format, uint32_t width, uint32_t height,
                       std::shared_ptr<const GpuPicture> picture, Timestamp capture_time,
                       ColorSpace color_space) noexcept
    : format_(format),
      width_(width),
      height_(height),
      capture_time_(capture_time),
      color_space_(color_space),
      gpu_(std::move(picture)) {}

MutableVideoFrame MutableVideoFrame::allocate(PixelFormat format, uint32_t width, uint32_t height) {
    return allocate(format, width, height, BufferPool::shared());
}

MutableVideoFrame MutableVideoFrame::allocate(PixelFormat format, uint32_t width, uint32_t height,
                                              BufferPool& pool) {
    assert(width > 0 && height > 0);
    MutableVideoFrame frame;
    frame.format_ = format;
    frame.width_ = width;
    frame.height_ = height;

    std::array<size_t, VideoFrame::kMaxPlanes> offsets{};
    size_t size = 0;
    for (size_t i = 0; i < plane_count(format); ++i) {
        frame.planes_[i].stride = align_up(plane_row_bytes(format, i, width));
        offsets[i] = size;
        size += align_up(frame.planes_[i].stride * plane_rows(format, i, height));
    }

    // Room to move the start up to the next boundary, and a boundary's worth
    // past the end for vectorised code that reads a little beyond a row.
    frame.storage_ = MutableBuffer::allocate(size + 2 * kAlignment, 0, pool);
    uint8_t* raw = frame.storage_.append(size + 2 * kAlignment);
    uint8_t* base = raw + (kAlignment - reinterpret_cast<uintptr_t>(raw) % kAlignment) % kAlignment;
    for (size_t i = 0; i < plane_count(format); ++i) frame.planes_[i].data = base + offsets[i];
    return frame;
}

VideoFrame MutableVideoFrame::freeze(Timestamp capture_time, ColorSpace color_space) && noexcept {
    VideoFrame frame;
    frame.format_ = format_;
    frame.width_ = width_;
    frame.height_ = height_;
    frame.capture_time_ = capture_time;
    frame.color_space_ = color_space;
    for (size_t i = 0; i < VideoFrame::kMaxPlanes; ++i) {
        frame.planes_[i] = VideoFrame::Plane{planes_[i].data, planes_[i].stride};
    }
    frame.storage_ = std::move(storage_).freeze();
    *this = MutableVideoFrame{};
    return frame;
}

Result<VideoFrame> download_frame(const VideoFrame& picture, BufferPool& pool) {
    if (!picture.gpu()) return picture;
    MutableVideoFrame copy =
        MutableVideoFrame::allocate(picture.format(), picture.width(), picture.height(), pool);
    if (Result<void> read = picture.gpu()->read(copy); !read) return fail(read.error());
    return std::move(copy).freeze(picture.capture_time(), picture.color_space());
}

Result<VideoFrame> download_frame(const VideoFrame& picture) {
    return download_frame(picture, BufferPool::shared());
}

}  // namespace ratcast
