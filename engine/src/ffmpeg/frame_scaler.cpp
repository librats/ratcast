// FrameScaler through libswscale: one 4:2:0 picture into another of a
// different size, its format and values as they are (no colour conversion:
// the same matrix and range on both sides).

#include <memory>
#include <utility>

extern "C" {
#include <libswscale/swscale.h>
}

#include "ffmpeg/av.h"
#include "ratcast/engine/video_convert.h"

namespace ratcast {

struct FrameScaler::State {
    struct ContextDeleter {
        void operator()(SwsContext* context) const noexcept { sws_freeContext(context); }
    };
    std::unique_ptr<SwsContext, ContextDeleter> context;
};

FrameScaler::FrameScaler() : state_(std::make_unique<State>()) {}

FrameScaler::~FrameScaler() = default;

Result<VideoFrame> FrameScaler::scale(const VideoFrame& picture, uint32_t width, uint32_t height) {
    if (picture.empty() || width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0)
        return fail(Errc::InvalidArgument);
    if (picture.format() != PixelFormat::NV12 && picture.format() != PixelFormat::I420)
        return fail(Errc::Unsupported);
    Result<VideoFrame> in_memory = download_frame(picture, BufferPool::shared());
    if (!in_memory) return fail(in_memory.error());
    const VideoFrame& source = *in_memory;

    const AVPixelFormat format = av::pixel_format(source.format());
    // Area-weighted: each sample made of all those it covers, as a picture
    // made two or three times smaller wants.
    state_->context.reset(sws_getCachedContext(
        state_->context.release(), int(source.width()), int(source.height()), format, int(width),
        int(height), format, SWS_AREA, nullptr, nullptr, nullptr));
    if (!state_->context) return fail(Errc::Unsupported);
    const ColorSpace colors = source.color_space();
    const int* table =
        sws_getCoefficients(colors.matrix == ColorMatrix::BT601 ? SWS_CS_ITU601 : SWS_CS_ITU709);
    const int full = colors.range == ColorRange::Full ? 1 : 0;
    sws_setColorspaceDetails(state_->context.get(), table, full, table, full, 0, 1 << 16, 1 << 16);

    const uint8_t* from[VideoFrame::kMaxPlanes] = {};
    int from_strides[VideoFrame::kMaxPlanes] = {};
    for (size_t i = 0; i < source.plane_count(); ++i) {
        from[i] = source.plane(i).data;
        from_strides[i] = int(source.plane(i).stride);
    }
    MutableVideoFrame out =
        MutableVideoFrame::allocate(source.format(), width, height, BufferPool::shared());
    uint8_t* to[VideoFrame::kMaxPlanes] = {};
    int to_strides[VideoFrame::kMaxPlanes] = {};
    for (size_t i = 0; i < source.plane_count(); ++i) {
        to[i] = out.plane(i).data;
        to_strides[i] = int(out.plane(i).stride);
    }
    sws_scale(state_->context.get(), from, from_strides, 0, int(source.height()), to, to_strides);
    return std::move(out).freeze(source.capture_time(), colors);
}

}  // namespace ratcast
