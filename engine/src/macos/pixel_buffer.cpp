#include "macos/pixel_buffer.h"

#include <CoreFoundation/CoreFoundation.h>

#include <cstring>
#include <memory>
#include <span>

namespace ratcast::macos {
namespace {

bool is_nv12(OSType type) {
    return type == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
           type == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
}

class CoreVideoDevice final : public GpuDevice {
public:
    GpuApi api() const noexcept override { return GpuApi::CoreVideo; }
};

/// A pixel buffer held, unlocked, as a picture in GPU memory.
class PixelBufferPicture final : public GpuPicture {
public:
    explicit PixelBufferPicture(CVPixelBufferRef pixels) : pixels_(CVPixelBufferRetain(pixels)) {}
    ~PixelBufferPicture() override { CVPixelBufferRelease(pixels_); }
    PixelBufferPicture(const PixelBufferPicture&) = delete;
    PixelBufferPicture& operator=(const PixelBufferPicture&) = delete;

    const std::shared_ptr<GpuDevice>& device() const noexcept override {
        return core_video_device();
    }

    Result<void> read(MutableVideoFrame& into) const override {
        const VideoFrame in_place = mapped();
        if (in_place.empty() || into.format() != in_place.format() ||
            into.width() != in_place.width() || into.height() != in_place.height()) {
            return fail(Errc::Internal);
        }
        for (size_t i = 0; i < in_place.plane_count(); ++i) {
            const VideoFrame::Plane& from = in_place.plane(i);
            const MutableVideoFrame::Plane& to = into.plane(i);
            const size_t bytes = plane_row_bytes(into.format(), i, into.width());
            for (size_t y = 0; y < plane_rows(into.format(), i, into.height()); ++y)
                std::memcpy(to.data + y * to.stride, from.data + y * from.stride, bytes);
        }
        return {};
    }

    // Locked for reading: the CPU sees what the GPU drew, where it drew it.
    VideoFrame mapped() const override { return frame_of(pixels_, Timestamp{}, ColorSpace{}); }

    CVPixelBufferRef pixels() const noexcept { return pixels_; }

private:
    CVPixelBufferRef const pixels_;
};

}  // namespace

VideoFrame frame_of(CVPixelBufferRef pixels, Timestamp captured, ColorSpace color_space) {
    const OSType type = CVPixelBufferGetPixelFormatType(pixels);
    const bool bgra = type == kCVPixelFormatType_32BGRA;
    if (!bgra && !is_nv12(type)) return {};
    if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
        return {};
    CVPixelBufferRetain(pixels);
    std::shared_ptr<const void> owner(static_cast<const void*>(pixels), [](const void* held) {
        auto buffer = static_cast<CVPixelBufferRef>(const_cast<void*>(held));
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(buffer);
    });

    uint32_t width = uint32_t(CVPixelBufferGetWidth(pixels));
    uint32_t height = uint32_t(CVPixelBufferGetHeight(pixels));
    VideoFrame::Plane planes[2];
    if (bgra) {
        planes[0] = {static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixels)),
                     CVPixelBufferGetBytesPerRow(pixels)};
    } else {
        width &= ~1u;
        height &= ~1u;
        for (size_t i = 0; i < 2; ++i) {
            planes[i] = {static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, i)),
                         CVPixelBufferGetBytesPerRowOfPlane(pixels, i)};
        }
    }
    if (width == 0 || height == 0 || !planes[0].data || (!bgra && !planes[1].data)) return {};
    const PixelFormat format = bgra ? PixelFormat::BGRA : PixelFormat::NV12;
    return VideoFrame(format, width, height, std::span(planes, plane_count(format)),
                      std::move(owner), captured,
                      bgra ? ColorSpace{ColorMatrix::BT709, ColorRange::Full} : color_space);
}

VideoFrame gpu_frame_of(CVPixelBufferRef pixels, Timestamp captured, ColorSpace color_space) {
    const size_t width = CVPixelBufferGetWidth(pixels);
    const size_t height = CVPixelBufferGetHeight(pixels);
    if (!is_nv12(CVPixelBufferGetPixelFormatType(pixels)) || !CVPixelBufferGetIOSurface(pixels) ||
        width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0) {
        return {};
    }
    return VideoFrame(PixelFormat::NV12, uint32_t(width), uint32_t(height),
                      std::make_shared<const PixelBufferPicture>(pixels), captured, color_space);
}

CVPixelBufferRef pixel_buffer_of(const VideoFrame& picture) noexcept {
    if (!picture.gpu() || picture.gpu()->device()->api() != GpuApi::CoreVideo) return nullptr;
    return static_cast<const PixelBufferPicture&>(*picture.gpu()).pixels();
}

const std::shared_ptr<GpuDevice>& core_video_device() {
    static const std::shared_ptr<GpuDevice> device = std::make_shared<CoreVideoDevice>();
    return device;
}

ColorSpace color_space_of(CVPixelBufferRef pixels) {
    ColorSpace color_space{ColorMatrix::BT601, ColorRange::Limited};
    if (CVPixelBufferGetPixelFormatType(pixels) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
        color_space.range = ColorRange::Full;
    const CFTypeRef matrix = CVBufferCopyAttachment(pixels, kCVImageBufferYCbCrMatrixKey, nullptr);
    if (matrix) {
        if (CFGetTypeID(matrix) == CFStringGetTypeID() &&
            CFStringCompare(static_cast<CFStringRef>(matrix), kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                            0) == kCFCompareEqualTo) {
            color_space.matrix = ColorMatrix::BT709;
        }
        CFRelease(matrix);
    }
    return color_space;
}

}  // namespace ratcast::macos
