#pragma once

/**
 * @file texture_reader.h
 * @brief A screen capture's BGRA textures as pictures in the format asked for.
 *
 * What Windows.Graphics.Capture and DXGI Desktop Duplication both hand out is
 * a BGRA texture of the GPU's, lent for a moment. Each is turned into NV12 by
 * the GPU and kept where it is when GPU pictures were asked for
 * (Nv12Converter); otherwise, or where the GPU cannot, it is copied into
 * memory the CPU reads and converted from there.
 */

#include <cstdint>
#include <memory>
#include <optional>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/video_frame.h"
#include "windows/d3d11.h"
#include "windows/nv12_converter.h"

namespace ratcast::d3d11 {

class TextureReader {
public:
    /// Pictures of `format` in `color_space`, kept on `device` when `gpu` and
    /// the device can convert them.
    TextureReader(std::shared_ptr<Device> device, PixelFormat format, ColorSpace color_space,
                  bool gpu);
    ~TextureReader();

    /// The top-left `width` x `height` of `texture`, one of the device's — made
    /// even for a 4:2:0 format — as a picture captured at `captured`; nullopt
    /// when there is none to give (every GPU picture still held, nothing left
    /// of the size). Fails when the GPU does.
    Result<std::optional<VideoFrame>> read(ID3D11Texture2D& texture, uint32_t width,
                                           uint32_t height, Timestamp captured);

    const std::shared_ptr<Device>& device() const noexcept { return device_; }

private:
    const std::shared_ptr<Device> device_;
    const PixelFormat format_;
    const ColorSpace color_space_;
    std::unique_ptr<Nv12Converter> converter_;  ///< for pictures kept in GPU memory
    win::ComPtr<ID3D11Texture2D> staging_;      ///< what the CPU reads a picture from
    uint32_t staging_width_ = 0;
    uint32_t staging_height_ = 0;
    DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
};

}  // namespace ratcast::d3d11
