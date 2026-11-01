// Android's AHardwareBuffers as pictures: see hardware_picture.h.

#include "android/hardware_picture.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace ratcast::android {
namespace {

constexpr uint32_t kBytesPerPixel = 4;

class HardwareBufferDevice final : public GpuDevice {
public:
    GpuApi api() const noexcept override { return GpuApi::HardwareBuffer; }
};

/// `buffer` unlocked, and let go of, with the last frame over its memory.
std::shared_ptr<const void> unlocking(AHardwareBuffer* buffer) {
    AHardwareBuffer_acquire(buffer);
    return {static_cast<const void*>(buffer), [](const void* held) {
                auto* locked = static_cast<AHardwareBuffer*>(const_cast<void*>(held));
                AHardwareBuffer_unlock(locked, nullptr);
                AHardwareBuffer_release(locked);
            }};
}

/// One plane of a YUV picture as the CPU sees it.
struct YuvPlane {
    const uint8_t* data = nullptr;
    size_t pixel_step = 0;
    size_t row_step = 0;
};

/// A buffer held, unlocked, as a picture in GPU memory: RGBA, or YUV 4:2:0
/// in whatever layout its allocator chose — read through its image where it
/// has one (Android finds a vendor's planes; locking the buffer does not) —
/// seen as NV12.
class HardwareBufferPicture final : public GpuPicture {
public:
    HardwareBufferPicture(AHardwareBuffer* buffer, AImage* image, std::shared_ptr<const void> owner,
                          PixelFormat format, uint32_t width, uint32_t height)
        : buffer_(buffer),
          image_(image),
          owner_(std::move(owner)),
          format_(format),
          width_(width),
          height_(height) {
        AHardwareBuffer_acquire(buffer_);
    }
    ~HardwareBufferPicture() override { AHardwareBuffer_release(buffer_); }
    HardwareBufferPicture(const HardwareBufferPicture&) = delete;
    HardwareBufferPicture& operator=(const HardwareBufferPicture&) = delete;

    const std::shared_ptr<GpuDevice>& device() const noexcept override {
        return hardware_buffer_device();
    }

    Result<void> read(MutableVideoFrame& into) const override {
        if (into.format() != format_ || into.width() != width_ || into.height() != height_)
            return fail(Errc::Internal);
        if (format_ == PixelFormat::RGBA) {
            const VideoFrame in_place = mapped();
            if (in_place.empty()) return fail(Errc::Internal);
            for (uint32_t y = 0; y < height_; ++y) {
                std::memcpy(into.plane(0).data + y * into.plane(0).stride,
                            in_place.plane(0).data + y * in_place.plane(0).stride,
                            size_t(width_) * kBytesPerPixel);
            }
            return {};
        }
        // YUV, sample by sample in its own steps.
        std::shared_ptr<const void> unlock;
        YuvPlane planes[3];
        if (!yuv_planes(planes, unlock)) return fail(Errc::Internal);
        const YuvPlane& y = planes[0];
        for (uint32_t row = 0; row < height_; ++row) {
            const uint8_t* from = y.data + row * y.row_step;
            uint8_t* to = into.plane(0).data + row * into.plane(0).stride;
            if (y.pixel_step == 1) {
                std::memcpy(to, from, width_);
                continue;
            }
            for (uint32_t column = 0; column < width_; ++column)
                to[column] = from[column * y.pixel_step];
        }
        const YuvPlane& u = planes[1];
        const YuvPlane& v = planes[2];
        for (uint32_t row = 0; row < height_ / 2; ++row) {
            const uint8_t* from_u = u.data + row * u.row_step;
            const uint8_t* from_v = v.data + row * v.row_step;
            uint8_t* to = into.plane(1).data + row * into.plane(1).stride;
            for (uint32_t column = 0; column < width_ / 2; ++column) {
                to[2 * column] = from_u[column * u.pixel_step];
                to[2 * column + 1] = from_v[column * v.pixel_step];
            }
        }
        return {};
    }

    // Locked for reading, until the frame's last copy goes: the CPU sees what
    // the GPU drew once it is done drawing. YUV only where it is NV12's
    // layout; elsewhere read() copies it into that.
    VideoFrame mapped() const override {
        if (format_ == PixelFormat::RGBA) {
            void* data = nullptr;
            AHardwareBuffer_Desc desc{};
            AHardwareBuffer_describe(buffer_, &desc);
            if (AHardwareBuffer_lock(buffer_, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr,
                                     &data) != 0 ||
                !data) {
                return {};
            }
            const VideoFrame::Plane plane{static_cast<const uint8_t*>(data),
                                          size_t(desc.stride) * kBytesPerPixel};
            return VideoFrame(PixelFormat::RGBA, width_, height_, {&plane, 1}, unlocking(buffer_),
                              Timestamp{});
        }
        std::shared_ptr<const void> unlock;
        YuvPlane p[3];
        if (!yuv_planes(p, unlock) || p[0].pixel_step != 1 || p[1].pixel_step != 2 ||
            p[2].pixel_step != 2 || p[1].row_step != p[2].row_step || p[2].data != p[1].data + 1) {
            return {};
        }
        const VideoFrame::Plane both[2] = {{p[0].data, p[0].row_step}, {p[1].data, p[1].row_step}};
        // Kept by the lock of the buffer, or by the image the planes are of.
        return VideoFrame(PixelFormat::NV12, width_, height_, both,
                          unlock ? std::move(unlock) : owner_, Timestamp{});
    }

    AHardwareBuffer* buffer() const noexcept { return buffer_; }

private:
    /// Where the YUV planes are: through the image, which keeps them read
    /// for as long as it lives, or the buffer locked (and `unlock` then
    /// unlocks it).
    bool yuv_planes(YuvPlane (&planes)[3], std::shared_ptr<const void>& unlock) const {
        if (image_) {
            for (int i = 0; i < 3; ++i) {
                uint8_t* data = nullptr;
                int length = 0;
                int32_t pixel_step = 0;
                int32_t row_step = 0;
                if (AImage_getPlaneData(image_, i, &data, &length) != AMEDIA_OK ||
                    AImage_getPlanePixelStride(image_, i, &pixel_step) != AMEDIA_OK ||
                    AImage_getPlaneRowStride(image_, i, &row_step) != AMEDIA_OK || !data ||
                    pixel_step <= 0 || row_step <= 0) {
                    return false;
                }
                planes[i] = {data, size_t(pixel_step), size_t(row_step)};
            }
            return true;
        }
        AHardwareBuffer_Planes locked{};
        if (AHardwareBuffer_lockPlanes(buffer_, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr,
                                       &locked) != 0) {
            return false;
        }
        unlock = unlocking(buffer_);
        if (locked.planeCount < 3) return false;
        for (int i = 0; i < 3; ++i) {
            const AHardwareBuffer_Plane& plane = locked.planes[i];
            if (!plane.data || plane.pixelStride == 0 || plane.rowStride == 0) return false;
            planes[i] = {static_cast<const uint8_t*>(plane.data), plane.pixelStride,
                         plane.rowStride};
        }
        return true;
    }

    AHardwareBuffer* const buffer_;
    AImage* const image_;  ///< kept by the owner, if any
    const std::shared_ptr<const void> owner_;
    const PixelFormat format_;
    const uint32_t width_;
    const uint32_t height_;
};

VideoFrame frame_of(AHardwareBuffer* buffer, AImage* image, std::shared_ptr<const void> owner,
                    uint32_t width, uint32_t height, Timestamp captured, ColorSpace color_space) {
    if (!buffer || width == 0 || height == 0) return {};
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer, &desc);
    PixelFormat format;
    if (desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
        desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM) {
        format = PixelFormat::RGBA;
    } else if (width % 2 == 0 && height % 2 == 0) {
        // Y8Cb8Cr8_420, or a vendor's own 4:2:0 (Qualcomm's decoders').
        format = PixelFormat::NV12;
    } else {
        return {};
    }
    if (desc.width < width || desc.height < height) return {};
    return VideoFrame(format, width, height,
                      std::make_shared<HardwareBufferPicture>(buffer, image, std::move(owner),
                                                              format, width, height),
                      captured, color_space);
}

}  // namespace

const std::shared_ptr<GpuDevice>& hardware_buffer_device() {
    static const std::shared_ptr<GpuDevice> device = std::make_shared<HardwareBufferDevice>();
    return device;
}

VideoFrame gpu_frame_of(AImage* image, std::shared_ptr<const void> owner, Timestamp captured,
                        ColorSpace color_space) {
    AImageCropRect crop{};
    AHardwareBuffer* buffer = nullptr;
    if (!image || AImage_getCropRect(image, &crop) != AMEDIA_OK ||
        AImage_getHardwareBuffer(image, &buffer) != AMEDIA_OK || crop.left != 0 || crop.top != 0) {
        return {};
    }
    // 4:2:0 wants whole pairs, and RGBA becomes it: an odd last row or column
    // is left out.
    return frame_of(buffer, image, std::move(owner), uint32_t(std::max(crop.right, 0)) & ~1u,
                    uint32_t(std::max(crop.bottom, 0)) & ~1u, captured, color_space);
}

AHardwareBuffer* hardware_buffer_of(const VideoFrame& picture) noexcept {
    const auto* ours = dynamic_cast<const HardwareBufferPicture*>(picture.gpu().get());
    return ours ? ours->buffer() : nullptr;
}

Result<VideoFrame> upload(const VideoFrame& picture) {
    if (picture.empty() || picture.gpu() || picture.format() != PixelFormat::RGBA)
        return fail(Errc::InvalidArgument);
    AHardwareBuffer_Desc desc{};
    desc.width = picture.width();
    desc.height = picture.height();
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY |
                 AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
    AHardwareBuffer* made = nullptr;
    if (AHardwareBuffer_allocate(&desc, &made) != 0 || !made) return fail(Errc::Unavailable);
    const std::unique_ptr<AHardwareBuffer, decltype(&AHardwareBuffer_release)> buffer(
        made, AHardwareBuffer_release);
    AHardwareBuffer_describe(buffer.get(), &desc);
    void* data = nullptr;
    if (AHardwareBuffer_lock(buffer.get(), AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, nullptr,
                             &data) != 0 ||
        !data) {
        return fail(Errc::Unavailable);
    }
    for (uint32_t y = 0; y < picture.height(); ++y) {
        std::memcpy(static_cast<uint8_t*>(data) + size_t(y) * desc.stride * kBytesPerPixel,
                    picture.plane(0).data + y * picture.plane(0).stride,
                    size_t(picture.width()) * kBytesPerPixel);
    }
    AHardwareBuffer_unlock(buffer.get(), nullptr);
    VideoFrame frame = frame_of(buffer.get(), nullptr, nullptr, picture.width(), picture.height(),
                                picture.capture_time(), picture.color_space());
    if (frame.empty()) return fail(Errc::Unavailable);
    return frame;
}

}  // namespace ratcast::android
