// FFmpeg's D3D11 frames on the engine's D3D11 devices (windows/d3d11.h). The
// device context wraps the capture's own device and takes its lock, so that
// FFmpeg, encoders and capture never use the immediate context at once.

#include "ffmpeg/hw_frames.h"

#include <d3d11_4.h>  // outside extern "C", where FFmpeg's header would include it

#include <cstdint>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include "ratcast/engine/video_convert.h"
#include "windows/d3d11.h"

#include "librats/util/logger.h"

namespace ratcast::av {
namespace {

/// Frames in a context's pool: an encoder holds one while it works on it
/// (none at all once it returns, at zero latency), and one opens alongside
/// for a bitrate change.
constexpr int kFrames = 8;

void lock(void* multithread) { static_cast<ID3D11Multithread*>(multithread)->Enter(); }
void unlock(void* multithread) { static_cast<ID3D11Multithread*>(multithread)->Leave(); }

ID3D11Device* device_of(const AVBufferRef& frames) noexcept {
    const auto& context = *reinterpret_cast<const AVHWFramesContext*>(frames.data);
    return static_cast<const AVD3D11VADeviceContext*>(context.device_ctx->hwctx)->device;
}

/// `picture`, in memory, uploaded into `frame`.
Result<void> upload(const VideoFrame& picture, AVFrame& frame) {
    Result<VideoFrame> nv12 = picture.format() == PixelFormat::NV12 && !picture.gpu()
                                  ? Result<VideoFrame>(picture)
                                  : convert_frame(picture, PixelFormat::NV12);
    if (!nv12) return fail(nv12.error());
    FramePtr source(av_frame_alloc());
    if (!source) return fail(Errc::Internal);
    source->format = AV_PIX_FMT_NV12;
    source->width = int(nv12->width());
    source->height = int(nv12->height());
    for (size_t i = 0; i < nv12->plane_count(); ++i) {
        source->data[i] = const_cast<uint8_t*>(nv12->plane(i).data);
        source->linesize[i] = int(nv12->plane(i).stride);
    }
    if (const int error = av_hwframe_transfer_data(&frame, source.get(), 0); error < 0) {
        LOG_WARN("engine", "a picture would not upload: " << describe(error));
        return fail(errc_of(error));
    }
    return {};
}

}  // namespace

bool takes_hw_frames(const AVCodec& codec, const GpuDevice& device) noexcept {
    if (device.api() != GpuApi::D3D11) return false;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(&codec, i);
        if (!config) return false;
        if (config->pix_fmt == AV_PIX_FMT_D3D11 &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX)) {
            return true;
        }
    }
}

Result<BufferRefPtr> make_hw_frames(const std::shared_ptr<GpuDevice>& device, uint32_t width,
                                    uint32_t height) {
    if (device->api() != GpuApi::D3D11) return fail(Errc::Unsupported);
    const auto& d3d = static_cast<const d3d11::Device&>(*device);
    if (!d3d.video()) return fail(Errc::Unavailable);  // FFmpeg's frames need the video API

    BufferRefPtr device_ref(av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA));
    if (!device_ref) return fail(Errc::Internal);
    auto& device_context = *reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
    auto& d3d_context = *static_cast<AVD3D11VADeviceContext*>(device_context.hwctx);
    d3d_context.device = d3d.get();
    d3d_context.device->AddRef();  // FFmpeg releases it
    ID3D11Multithread* multithread = d3d.multithread();
    multithread->AddRef();
    d3d_context.lock = lock;
    d3d_context.unlock = unlock;
    d3d_context.lock_ctx = multithread;
    device_context.user_opaque = multithread;
    device_context.free = [](AVHWDeviceContext* context) {
        static_cast<ID3D11Multithread*>(context->user_opaque)->Release();
    };
    if (const int error = av_hwdevice_ctx_init(device_ref.get()); error < 0) {
        LOG_DEBUG("engine", "no D3D11 device for FFmpeg: " << describe(error));
        return fail(errc_of(error));
    }

    BufferRefPtr frames(av_hwframe_ctx_alloc(device_ref.get()));
    if (!frames) return fail(Errc::Internal);
    auto& frames_context = *reinterpret_cast<AVHWFramesContext*>(frames->data);
    frames_context.format = AV_PIX_FMT_D3D11;
    frames_context.sw_format = AV_PIX_FMT_NV12;
    frames_context.width = int(width);
    frames_context.height = int(height);
    frames_context.initial_pool_size = kFrames;
    // Bound as a decoder's: an array of NV12 textures takes no other binding
    // (E_INVALIDARG for none or a render target's), and a decoder's frames
    // are what hardware encoders are made to take.
    static_cast<AVD3D11VAFramesContext*>(frames_context.hwctx)->BindFlags = D3D11_BIND_DECODER;
    if (const int error = av_hwframe_ctx_init(frames.get()); error < 0) {
        LOG_DEBUG("engine",
                  "no D3D11 frames of " << width << "x" << height << ": " << describe(error));
        return fail(errc_of(error));
    }
    return frames;
}

Result<void> fill_hw_frame(AVBufferRef& frames, const VideoFrame& picture, AVFrame& frame) {
    if (const int error = av_hwframe_get_buffer(&frames, &frame, 0); error < 0) {
        LOG_WARN("engine", "no D3D11 frame free: " << describe(error));
        return fail(errc_of(error));
    }
    const d3d11::Picture* gpu = d3d11::picture_of(picture);
    if (!gpu || gpu->d3d11().get() != device_of(frames)) return upload(picture, frame);

    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame.data[0]);
    const auto index = static_cast<UINT>(reinterpret_cast<intptr_t>(frame.data[1]));
    const d3d11::Lock lock(gpu->d3d11());
    gpu->d3d11().context()->CopySubresourceRegion(texture, index, 0, 0, 0, gpu->texture(),
                                                  gpu->index(), nullptr);
    return {};
}

}  // namespace ratcast::av
