#include "windows/d3d11.h"

#include <cstring>
#include <utility>
#include <vector>

#include "librats/util/logger.h"

namespace ratcast::d3d11 {
namespace {

using win::ComPtr;

HRESULT create_device(D3D_DRIVER_TYPE type, UINT flags, ComPtr<ID3D11Device>& device,
                      ComPtr<ID3D11DeviceContext>& context) {
    return D3D11CreateDevice(nullptr, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device,
                             nullptr, &context);
}

/// `rows` rows of `row_bytes` each, from one stride to another.
void copy_rows(const uint8_t* from, size_t from_stride, uint8_t* to, size_t to_stride,
               size_t row_bytes, size_t rows) {
    for (size_t y = 0; y < rows; ++y)
        std::memcpy(to + y * to_stride, from + y * from_stride, row_bytes);
}

}  // namespace

Result<std::shared_ptr<Device>> Device::create() {
    std::shared_ptr<Device> device(new Device);
    // BGRA for what Windows.Graphics.Capture gives; video for the GPU's own
    // colour conversion and FFmpeg's frames, where the driver has it.
    constexpr UINT kBgra = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT result =
        create_device(D3D_DRIVER_TYPE_HARDWARE, kBgra | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                      device->device_, device->context_);
    if (FAILED(result))
        result = create_device(D3D_DRIVER_TYPE_HARDWARE, kBgra, device->device_, device->context_);
    if (FAILED(result))
        result = create_device(D3D_DRIVER_TYPE_WARP, kBgra, device->device_, device->context_);
    if (FAILED(result)) {
        LOG_WARN("engine", "no Direct3D 11 device: 0x" << std::hex << unsigned(result));
        return fail(win::errc_of(result));
    }
    if (FAILED(device->context_.As(&device->multithread_))) return fail(Errc::Unavailable);
    device->multithread_->SetMultithreadProtected(TRUE);
    ComPtr<ID3D11VideoDevice> video;
    device->video_ = SUCCEEDED(device->device_.As(&video));
    return device;
}

Result<void> Device::read(ID3D11Texture2D& texture, uint32_t index, MutableVideoFrame& into) const {
    if (into.format() != PixelFormat::NV12) return fail(Errc::Unsupported);
    const uint32_t width = into.width();
    const uint32_t height = into.height();

    const Lock lock(*this);
    D3D11_TEXTURE2D_DESC staging_desc{};
    if (staging_) staging_->GetDesc(&staging_desc);
    if (!staging_ || staging_desc.Width != width || staging_desc.Height != height) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_.Reset();
        if (const HRESULT result = device_->CreateTexture2D(&desc, nullptr, &staging_);
            FAILED(result)) {
            return fail(win::errc_of(result));
        }
    }
    D3D11_TEXTURE2D_DESC desc;
    texture.GetDesc(&desc);
    // Planar formats are copied whole, both planes at once: no box.
    if (desc.Width == width && desc.Height == height) {
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, &texture,
                                        D3D11CalcSubresource(0, index, desc.MipLevels), nullptr);
    } else {
        const D3D11_BOX box{0, 0, 0, width, height, 1};
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, &texture,
                                        D3D11CalcSubresource(0, index, desc.MipLevels), &box);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (const HRESULT result = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        FAILED(result)) {
        return fail(win::errc_of(result));
    }
    const auto* luma = static_cast<const uint8_t*>(mapped.pData);
    copy_rows(luma, mapped.RowPitch, into.plane(0).data, into.plane(0).stride,
              plane_row_bytes(PixelFormat::NV12, 0, width), height);
    copy_rows(luma + size_t{mapped.RowPitch} * height, mapped.RowPitch, into.plane(1).data,
              into.plane(1).stride, plane_row_bytes(PixelFormat::NV12, 1, width),
              plane_rows(PixelFormat::NV12, 1, height));
    context_->Unmap(staging_.Get(), 0);
    return {};
}

Picture::Picture(std::shared_ptr<Device> device, ComPtr<ID3D11Texture2D> texture, uint32_t index,
                 std::shared_ptr<const void> owner) noexcept
    : device_(std::move(device)),
      texture_(std::move(texture)),
      index_(index),
      owner_(std::move(owner)) {}

Result<void> Picture::read(MutableVideoFrame& into) const {
    return d3d11().read(*texture_.Get(), index_, into);
}

const Picture* picture_of(const VideoFrame& frame) noexcept {
    if (!frame.gpu() || frame.gpu()->device()->api() != GpuApi::D3D11) return nullptr;
    return static_cast<const Picture*>(frame.gpu().get());
}

Result<VideoFrame> upload(const std::shared_ptr<Device>& device, const VideoFrame& picture) {
    if (picture.format() != PixelFormat::NV12 || picture.gpu()) return fail(Errc::Unsupported);
    const uint32_t width = picture.width();
    const uint32_t height = picture.height();
    // D3D11 takes an NV12 texture's contents as one block: the luma rows,
    // then the chroma rows, one pitch apart.
    const size_t pitch = plane_row_bytes(PixelFormat::NV12, 0, width);
    std::vector<uint8_t> contents(pitch * (height + plane_rows(PixelFormat::NV12, 1, height)));
    copy_rows(picture.plane(0).data, picture.plane(0).stride, contents.data(), pitch, pitch,
              height);
    copy_rows(picture.plane(1).data, picture.plane(1).stride, contents.data() + pitch * height,
              pitch, plane_row_bytes(PixelFormat::NV12, 1, width),
              plane_rows(PixelFormat::NV12, 1, height));

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    const D3D11_SUBRESOURCE_DATA data{contents.data(), UINT(pitch), 0};
    ComPtr<ID3D11Texture2D> texture;
    if (const HRESULT result = device->get()->CreateTexture2D(&desc, &data, &texture);
        FAILED(result)) {
        return fail(win::errc_of(result));
    }
    return VideoFrame(PixelFormat::NV12, width, height,
                      std::make_shared<Picture>(device, std::move(texture), 0),
                      picture.capture_time(), picture.color_space());
}

}  // namespace ratcast::d3d11
