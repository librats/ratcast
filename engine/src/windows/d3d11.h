#pragma once

/**
 * @file d3d11.h
 * @brief Direct3D 11 devices, and the pictures in their memory.
 *
 * A Device is the GpuDevice of D3D11: a device and its immediate context,
 * which capture, encoders and FFmpeg use from threads of their own. D3D11's
 * own lock (ID3D11Multithread) makes each call on the context safe; Lock
 * holds it over a run of calls that must not be interleaved with another
 * thread's — a copy and the map that reads it — and FFmpeg takes the same.
 *
 * A Picture is the GpuPicture of D3D11: an NV12 texture of the device's (one
 * slice of it), read back into memory through a staging texture.
 */

#include <d3d11_4.h>

#include <cstdint>
#include <memory>

#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"
#include "windows/com.h"

namespace ratcast::d3d11 {

class Device final : public GpuDevice {
public:
    /// A device on the system's default adapter, one that processes video if
    /// it can; WARP, the software one, where there is no GPU (a virtual
    /// machine).
    static Result<std::shared_ptr<Device>> create();

    GpuApi api() const noexcept override { return GpuApi::D3D11; }

    ID3D11Device* get() const noexcept { return device_.Get(); }
    ID3D11DeviceContext* context() const noexcept { return context_.Get(); }
    /// D3D11's lock over the immediate context: recursive.
    ID3D11Multithread* multithread() const noexcept { return multithread_.Get(); }

    /// Whether the device has the video API (ID3D11VideoDevice): a GPU's own
    /// colour conversion, and what FFmpeg's D3D11 frames need.
    bool video() const noexcept { return video_; }

    /// Copy slice `index` of the NV12 texture `texture` into `into`, of its
    /// size. Waits for the GPU.
    Result<void> read(ID3D11Texture2D& texture, uint32_t index, MutableVideoFrame& into) const;

private:
    Device() = default;

    win::ComPtr<ID3D11Device> device_;
    win::ComPtr<ID3D11DeviceContext> context_;
    win::ComPtr<ID3D11Multithread> multithread_;
    bool video_ = false;
    /// What read() copies into, kept for the next of its size; under the lock.
    mutable win::ComPtr<ID3D11Texture2D> staging_;
};

/// The device's lock, held for as long as this lives.
class Lock {
public:
    explicit Lock(const Device& device) noexcept : multithread_(device.multithread()) {
        multithread_->Enter();
    }
    ~Lock() { multithread_->Leave(); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    ID3D11Multithread* multithread_;
};

class Picture final : public GpuPicture {
public:
    /// Slice `index` of `texture`, an NV12 texture of `device`'s; `owner` is
    /// kept for as long as the picture is (a pool's slot, given back then).
    Picture(std::shared_ptr<Device> device, win::ComPtr<ID3D11Texture2D> texture, uint32_t index,
            std::shared_ptr<const void> owner = nullptr) noexcept;

    const std::shared_ptr<GpuDevice>& device() const noexcept override { return device_; }
    Result<void> read(MutableVideoFrame& into) const override;

    const Device& d3d11() const noexcept { return static_cast<const Device&>(*device_); }
    ID3D11Texture2D* texture() const noexcept { return texture_.Get(); }
    uint32_t index() const noexcept { return index_; }

private:
    std::shared_ptr<GpuDevice> device_;
    win::ComPtr<ID3D11Texture2D> texture_;
    uint32_t index_;
    std::shared_ptr<const void> owner_;
};

/// The D3D11 picture in `frame`, if that is what it carries.
const Picture* picture_of(const VideoFrame& frame) noexcept;

/// `picture`, an NV12 picture in memory, copied into a texture of `device`'s.
Result<VideoFrame> upload(const std::shared_ptr<Device>& device, const VideoFrame& picture);

}  // namespace ratcast::d3d11
