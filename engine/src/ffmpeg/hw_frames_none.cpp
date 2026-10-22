// No graphics API whose textures FFmpeg's encoders take here: pictures reach
// every encoder through memory.

#include "ffmpeg/hw_frames.h"

namespace ratcast::av {

bool takes_hw_frames(const AVCodec&, const GpuDevice&) noexcept { return false; }

Result<BufferRefPtr> make_hw_frames(const std::shared_ptr<GpuDevice>&, uint32_t, uint32_t) {
    return fail(Errc::Unavailable);
}

Result<void> fill_hw_frame(AVBufferRef&, const VideoFrame&, AVFrame&) {
    return fail(Errc::Unavailable);
}

}  // namespace ratcast::av
