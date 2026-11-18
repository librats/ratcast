#include "windows/texture_reader.h"

#include <utility>

#include "ratcast/engine/video_convert.h"

#include "librats/util/logger.h"

namespace ratcast::d3d11 {

TextureReader::TextureReader(std::shared_ptr<Device> device, PixelFormat format,
                             ColorSpace color_space, bool gpu)
    : device_(std::move(device)), format_(format), color_space_(color_space) {
    if (!gpu || format_ != PixelFormat::NV12) return;
    Result<std::unique_ptr<Nv12Converter>> converter = Nv12Converter::create(device_, color_space_);
    if (converter) {
        converter_ = std::move(*converter);
    } else {
        LOG_INFO("engine", "screen capture: pictures through memory, the GPU cannot convert them");
    }
}

TextureReader::~TextureReader() = default;

Result<std::optional<VideoFrame>> TextureReader::read(ID3D11Texture2D& texture, uint32_t width,
                                                      uint32_t height, Timestamp captured) {
    D3D11_TEXTURE2D_DESC desc;
    texture.GetDesc(&desc);
    width = std::min(width, desc.Width);
    height = std::min(height, desc.Height);
    if (format_ != PixelFormat::BGRA) {  // 4:2:0 wants even sizes
        width &= ~1u;
        height &= ~1u;
    }
    if (width == 0 || height == 0) return std::optional<VideoFrame>{};

    if (converter_) {
        Result<VideoFrame> picture = converter_->convert(texture, width, height, captured);
        if (picture && picture->empty()) return std::optional<VideoFrame>{};  // dropped
        if (picture) return std::optional<VideoFrame>{std::move(*picture)};
        // A size its video processor will not take, say: through memory from
        // now on, which says so if the GPU itself has gone.
        LOG_WARN("engine", "screen capture: the GPU stopped converting pictures, at "
                               << width << "x" << height);
        converter_.reset();
    }

    if (!staging_ || staging_width_ != width || staging_height_ != height ||
        staging_format_ != desc.Format) {
        D3D11_TEXTURE2D_DESC staging{};
        staging.Width = width;
        staging.Height = height;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.Format = desc.Format;
        staging.SampleDesc.Count = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_.Reset();
        if (const HRESULT result = device_->get()->CreateTexture2D(&staging, nullptr, &staging_);
            FAILED(result)) {
            return fail(win::errc_of(result));
        }
        staging_width_ = width;
        staging_height_ = height;
        staging_format_ = desc.Format;
    }
    const Lock lock(*device_);
    const D3D11_BOX box{0, 0, 0, width, height, 1};
    ID3D11DeviceContext& context = *device_->context();
    context.CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, &texture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (const HRESULT result = context.Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        FAILED(result)) {
        return fail(win::errc_of(result));
    }
    const VideoFrame::Plane plane{static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch};
    const VideoFrame view(PixelFormat::BGRA, width, height, {&plane, 1}, nullptr, captured);
    Result<VideoFrame> picture = convert_frame(view, format_, color_space_);
    context.Unmap(staging_.Get(), 0);
    if (!picture) return std::optional<VideoFrame>{};
    return std::optional<VideoFrame>{std::move(*picture)};
}

}  // namespace ratcast::d3d11
