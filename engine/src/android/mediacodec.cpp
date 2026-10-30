// What MediaCodec's decoder and encoder share: see android/mediacodec.h.

#include "android/mediacodec.h"

#include <media/NdkMediaCodec.h>

#include <algorithm>
#include <cstring>

namespace ratcast::android {
namespace {

/// The "image-data" of a codec's format: Android's MediaImage2
/// (media/hardware/VideoAPI.h), where each plane of a picture is in its
/// buffer.
struct MediaImage2 {
    struct Plane {
        uint32_t offset;
        int32_t column_step;
        int32_t row_step;
        uint32_t horizontal_subsampling;
        uint32_t vertical_subsampling;
    };
    int32_t type;  ///< 1: YUV
    uint32_t plane_count;
    uint32_t width;
    uint32_t height;
    uint32_t bit_depth;
    uint32_t bit_depth_allocated;
    Plane planes[4];
};
constexpr int32_t kMediaImageYuv = 1;

int32_t int_of(AMediaFormat* format, const char* key, int32_t otherwise) {
    int32_t value = 0;
    return AMediaFormat_getInt32(format, key, &value) ? value : otherwise;
}

}  // namespace

const char* mime_of(Codec codec) {
    switch (codec) {
        case Codec::H264: return "video/avc";
        case Codec::H265: return "video/hevc";
        case Codec::AV1: return "video/av01";
        default: return nullptr;
    }
}

bool is_software(std::string_view name) {
    return name.starts_with("c2.android.") || name.starts_with("OMX.google.");
}

std::string name_of(AMediaCodec* codec) {
    std::string name;
    if (char* own = nullptr; AMediaCodec_getName(codec, &own) == AMEDIA_OK && own) {
        name = own;
        AMediaCodec_releaseName(codec, own);
    }
    return name;
}

bool MediaLayout::fits(size_t size) const noexcept {
    // The last byte each plane's crop reaches.
    for (size_t i = 0; i < 3; ++i) {
        const Plane& plane = planes[i];
        const size_t shift = i == 0 ? 0 : 1;
        const size_t last_row = ((top + height) >> shift) - 1;
        const size_t last_column = ((left + width) >> shift) - 1;
        if (plane.row_step <= 0 || plane.column_step <= 0 ||
            plane.offset + last_row * size_t(plane.row_step) +
                    last_column * size_t(plane.column_step) >=
                size)
            return false;
    }
    return true;
}

MediaLayout layout_of(AMediaFormat* format) {
    MediaLayout layout;
    const int32_t width = int_of(format, AMEDIAFORMAT_KEY_WIDTH, 0);
    const int32_t height = int_of(format, AMEDIAFORMAT_KEY_HEIGHT, 0);
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = width - 1;
    int32_t bottom = height - 1;
    AMediaFormat_getRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, &left, &top, &right, &bottom);
    layout.left = uint32_t(std::max(0, left));
    layout.top = uint32_t(std::max(0, top));
    // 4:2:0 wants whole pairs: an odd last row or column is left out.
    layout.width = uint32_t(std::max(0, right - left + 1)) & ~1u;
    layout.height = uint32_t(std::max(0, bottom - top + 1)) & ~1u;

    void* image = nullptr;
    size_t image_size = 0;
    if (AMediaFormat_getBuffer(format, "image-data", &image, &image_size) &&
        image_size >= sizeof(MediaImage2)) {
        MediaImage2 described{};
        std::memcpy(&described, image, sizeof described);
        if (described.type == kMediaImageYuv && described.plane_count == 3 &&
            described.bit_depth == 8) {
            for (size_t i = 0; i < 3; ++i) {
                layout.planes[i] = {described.planes[i].offset, described.planes[i].column_step,
                                    described.planes[i].row_step};
            }
            layout.known = true;
        }
        return layout;
    }
    // No description: the two layouts a colour format names, by the stride
    // and the rows of the luma plane.
    const int32_t color = int_of(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0);
    const int32_t stride = std::max(int_of(format, AMEDIAFORMAT_KEY_STRIDE, width), width);
    const int32_t rows = std::max(int_of(format, AMEDIAFORMAT_KEY_SLICE_HEIGHT, height), height);
    const uint32_t luma = uint32_t(stride * rows);
    if (color == kColorFormatSemiPlanar) {
        layout.planes[0] = {0, 1, stride};
        layout.planes[1] = {luma, 2, stride};
        layout.planes[2] = {luma + 1, 2, stride};
        layout.known = true;
    } else if (color == kColorFormatPlanar) {
        const uint32_t chroma = uint32_t(stride / 2 * rows / 2);
        layout.planes[0] = {0, 1, stride};
        layout.planes[1] = {luma, 1, stride / 2};
        layout.planes[2] = {luma + chroma, 1, stride / 2};
        layout.known = true;
    }
    return layout;
}

}  // namespace ratcast::android
