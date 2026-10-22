#pragma once

/**
 * @file nv12_converter.h
 * @brief BGRA textures into NV12 pictures, on the GPU.
 *
 * The device's video processor (ID3D11VideoProcessor: fixed-function hardware
 * every GPU with a video engine has) turns what Windows.Graphics.Capture
 * gives into what hardware encoders take, in the colour space asked for,
 * without the picture leaving the GPU. Each picture is drawn into a texture
 * of a small pool, given back when the last frame that carries it goes; with
 * every one of them still held, there is no picture — the source drops it,
 * as capture APIs do whose buffers are not returned.
 */

#include <cstdint>
#include <memory>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/video_frame.h"
#include "windows/d3d11.h"

namespace ratcast::d3d11 {

class Nv12Converter {
public:
    /// Pictures alive at once, at most: one with the source, one waiting to
    /// be encoded, one being encoded and the one kept for a keyframe, and a
    /// spare or two.
    static constexpr size_t kPictures = 6;

    /// A converter on `device` into `color_space`. Fails with Unavailable
    /// where the device has no video processor that writes NV12 (WARP).
    static Result<std::unique_ptr<Nv12Converter>> create(std::shared_ptr<Device> device,
                                                         ColorSpace color_space);
    ~Nv12Converter();

    /// The top-left `width` x `height` — both even — of `bgra` as an NV12
    /// picture captured at `captured`; an empty frame when every picture of the
    /// pool is still held.
    Result<VideoFrame> convert(ID3D11Texture2D& bgra, uint32_t width, uint32_t height,
                               Timestamp captured);

private:
    struct State;
    explicit Nv12Converter(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

}  // namespace ratcast::d3d11
