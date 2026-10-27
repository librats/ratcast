#include "macos/pixel_buffer.h"

#include <CoreFoundation/CoreFoundation.h>

#include <memory>
#include <span>

namespace ratcast::macos {

VideoFrame frame_of(CVPixelBufferRef pixels, Timestamp captured, ColorSpace color_space) {
    const OSType type = CVPixelBufferGetPixelFormatType(pixels);
    const bool bgra = type == kCVPixelFormatType_32BGRA;
    if (!bgra && type != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
        type != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        return {};
    }
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
