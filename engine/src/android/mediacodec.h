#pragma once

/**
 * @file mediacodec.h
 * @brief What MediaCodec's decoder and encoder share: codecs by their MIME
 * type, the system's software ones told apart, and where a 4:2:0 picture's
 * planes are in a codec's buffer.
 *
 * A codec's format says where: Android's MediaImage2 ("image-data") where the
 * codec describes its buffers, else the stride and rows of the luma plane
 * for the two layouts a colour format names (NV12's semi-planar, I420's
 * planar). Decoders' pictures are cropped (the display crop); an encoder's
 * input is the whole size.
 */

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ratcast/stream/stream_description.h"

namespace ratcast::android {

/// The MIME type of `codec`'s video, or null for one MediaCodec has none of.
const char* mime_of(Codec codec);

/// The system's own software codecs (c2.android.*, OMX.google.*), which
/// FFmpeg's match: the engine takes the hardware's only.
bool is_software(std::string_view name);

/// The name of `codec`, or "" if it gives none.
std::string name_of(AMediaCodec* codec);

/// MediaCodecInfo.CodecCapabilities' colour formats.
constexpr int32_t kColorFormatPlanar = 19;      ///< COLOR_FormatYUV420Planar: I420
constexpr int32_t kColorFormatSemiPlanar = 21;  ///< COLOR_FormatYUV420SemiPlanar: NV12
/// COLOR_FormatYUV420Flexible: 4:2:0 in whatever layout the codec has, which
/// its format describes.
constexpr int32_t kColorFormatFlexible = 0x7F420888;

struct MediaLayout {
    struct Plane {
        uint32_t offset = 0;
        int32_t column_step = 0;  ///< bytes from one sample to the next
        int32_t row_step = 0;     ///< bytes from one row to the next
    };
    bool known = false;
    uint32_t left = 0;
    uint32_t top = 0;
    uint32_t width = 0;  ///< what is shown, even
    uint32_t height = 0;
    Plane planes[3];  ///< Y, U, V

    /// Whether U and V are interleaved as NV12's are, U first.
    bool nv12() const noexcept {
        return planes[1].column_step == 2 && planes[2].column_step == 2 &&
               planes[2].offset == planes[1].offset + 1 && planes[1].row_step == planes[2].row_step;
    }

    /// Whether every sample of the picture lies inside a buffer of `size`.
    bool fits(size_t size) const noexcept;
};

/// The layout `format` describes; `known` false if it describes none the
/// engine reads (more than 8 bits, not YUV).
MediaLayout layout_of(AMediaFormat* format);

}  // namespace ratcast::android
