// Pictures in Direct3D 11's memory: uploaded and read back as they were, made
// from BGRA by the GPU's video processor in the colours the CPU's conversion
// gives, from a pool that runs dry while its pictures are held. Skipped where
// there is no device, or no video processor (WARP).

#include "windows/d3d11.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/video_convert.h"
#include "windows/nv12_converter.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

/// Whether two pictures in memory hold the same samples, each within `tolerance`.
::testing::AssertionResult same_samples(const VideoFrame& a, const VideoFrame& b,
                                        int tolerance = 0) {
    if (a.format() != b.format() || a.width() != b.width() || a.height() != b.height())
        return ::testing::AssertionFailure() << "of another format or size";
    for (size_t i = 0; i < a.plane_count(); ++i) {
        for (size_t y = 0; y < plane_rows(a.format(), i, a.height()); ++y) {
            const uint8_t* row_a = a.plane(i).data + y * a.plane(i).stride;
            const uint8_t* row_b = b.plane(i).data + y * b.plane(i).stride;
            for (size_t x = 0; x < plane_row_bytes(a.format(), i, a.width()); ++x) {
                if (std::abs(int(row_a[x]) - int(row_b[x])) > tolerance) {
                    return ::testing::AssertionFailure()
                           << "plane " << i << " at " << x << "," << y << ": " << int(row_a[x])
                           << " against " << int(row_b[x]);
                }
            }
        }
    }
    return ::testing::AssertionSuccess();
}

/// A BGRA picture of 16 x 16 blocks, each one colour: chroma, an average of
/// 2 x 2 pixels, is the same however it is sited.
VideoFrame blocks(uint32_t width, uint32_t height) {
    MutableVideoFrame frame = MutableVideoFrame::allocate(PixelFormat::BGRA, width, height);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = frame.plane(0).data + y * frame.plane(0).stride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t block = (y / 16) * 7 + x / 16;
            row[x * 4 + 0] = uint8_t(block * 53);
            row[x * 4 + 1] = uint8_t(block * 97 + 40);
            row[x * 4 + 2] = uint8_t(255 - block * 31);
            row[x * 4 + 3] = 255;
        }
    }
    return std::move(frame).freeze(Timestamp{1s});
}

class D3D11Test : public ::testing::Test {
protected:
    void SetUp() override {
        Result<std::shared_ptr<d3d11::Device>> device = d3d11::Device::create();
        if (!device) GTEST_SKIP() << "no Direct3D 11 device";
        device_ = std::move(*device);
    }

    /// `picture`, BGRA in memory, in a texture of the device's.
    win::ComPtr<ID3D11Texture2D> texture_of(const VideoFrame& picture) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = picture.width();
        desc.Height = picture.height();
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{picture.plane(0).data, UINT(picture.plane(0).stride), 0};
        win::ComPtr<ID3D11Texture2D> texture;
        EXPECT_HRESULT_SUCCEEDED(device_->get()->CreateTexture2D(&desc, &data, &texture));
        return texture;
    }

    std::unique_ptr<d3d11::Nv12Converter> converter(ColorSpace color_space = {}) {
        Result<std::unique_ptr<d3d11::Nv12Converter>> converter =
            d3d11::Nv12Converter::create(device_, color_space);
        return converter ? std::move(*converter) : nullptr;
    }

    std::shared_ptr<d3d11::Device> device_;
};

}  // namespace

TEST_F(D3D11Test, APictureReadsBackAsItWasUploaded) {
    const VideoFrame picture = draw_pattern({PixelFormat::NV12, 320, 240}, 7, Timestamp{3s});
    Result<VideoFrame> uploaded = d3d11::upload(device_, picture);
    ASSERT_TRUE(uploaded);
    ASSERT_TRUE(uploaded->gpu());
    EXPECT_EQ(uploaded->plane(0).data, nullptr) << "no planes the CPU reads";
    EXPECT_EQ(uploaded->gpu()->device()->api(), GpuApi::D3D11);
    EXPECT_EQ(uploaded->capture_time(), Timestamp{3s});

    Result<VideoFrame> downloaded = download_frame(*uploaded);
    ASSERT_TRUE(downloaded);
    EXPECT_FALSE(downloaded->gpu());
    EXPECT_EQ(downloaded->capture_time(), Timestamp{3s});
    EXPECT_TRUE(same_samples(*downloaded, picture));
}

TEST_F(D3D11Test, ConvertingAGpuPictureReadsItFirst) {
    const VideoFrame picture = draw_pattern({PixelFormat::NV12, 320, 240}, 2, Timestamp{1s});
    Result<VideoFrame> uploaded = d3d11::upload(device_, picture);
    ASSERT_TRUE(uploaded);
    Result<VideoFrame> i420 = convert_frame(*uploaded, PixelFormat::I420);
    ASSERT_TRUE(i420);
    Result<VideoFrame> expected = convert_frame(picture, PixelFormat::I420);
    ASSERT_TRUE(expected);
    EXPECT_TRUE(same_samples(*i420, *expected));
    EXPECT_EQ(read_timestamp(*i420), Timestamp{1s});
}

TEST_F(D3D11Test, TheGpuConvertsInTheCpusColours) {
    const VideoFrame bgra = blocks(112, 64);
    const win::ComPtr<ID3D11Texture2D> texture = texture_of(bgra);
    for (const ColorSpace color_space : {ColorSpace{ColorMatrix::BT709, ColorRange::Limited},
                                         ColorSpace{ColorMatrix::BT601, ColorRange::Full}}) {
        const std::unique_ptr<d3d11::Nv12Converter> converter = this->converter(color_space);
        if (!converter) GTEST_SKIP() << "no video processor here";
        Result<VideoFrame> converted = converter->convert(*texture.Get(), 112, 64, Timestamp{1s});
        ASSERT_TRUE(converted);
        ASSERT_TRUE(converted->gpu());
        EXPECT_EQ(converted->color_space(), color_space);
        EXPECT_EQ(converted->capture_time(), Timestamp{1s});
        Result<VideoFrame> gpu = download_frame(*converted);
        ASSERT_TRUE(gpu);
        Result<VideoFrame> cpu = convert_frame(bgra, PixelFormat::NV12, color_space);
        ASSERT_TRUE(cpu);
        // Rounding, and a filter at the blocks' edges: a level or two.
        EXPECT_TRUE(same_samples(*gpu, *cpu, 3))
            << "matrix " << int(color_space.matrix) << ", range " << int(color_space.range);
    }
}

TEST_F(D3D11Test, TheGpuConvertsOnlyTheSizeAsked) {
    const VideoFrame bgra = blocks(112, 64);
    const win::ComPtr<ID3D11Texture2D> texture = texture_of(bgra);
    const std::unique_ptr<d3d11::Nv12Converter> converter = this->converter();
    if (!converter) GTEST_SKIP() << "no video processor here";
    Result<VideoFrame> converted = converter->convert(*texture.Get(), 64, 32, Timestamp{});
    ASSERT_TRUE(converted);
    EXPECT_EQ(converted->width(), 64u);
    EXPECT_EQ(converted->height(), 32u);
    Result<VideoFrame> gpu = download_frame(*converted);
    ASSERT_TRUE(gpu);
    // The top-left corner, not the whole picture made smaller.
    const VideoFrame::Plane corner{bgra.plane(0).data, bgra.plane(0).stride};
    Result<VideoFrame> cpu =
        convert_frame(VideoFrame(PixelFormat::BGRA, 64, 32, {&corner, 1}, nullptr, Timestamp{}),
                      PixelFormat::NV12);
    ASSERT_TRUE(cpu);
    EXPECT_TRUE(same_samples(*gpu, *cpu, 3));
}

TEST_F(D3D11Test, NoPictureWhileEveryOneIsHeld) {
    const win::ComPtr<ID3D11Texture2D> texture = texture_of(blocks(64, 64));
    const std::unique_ptr<d3d11::Nv12Converter> converter = this->converter();
    if (!converter) GTEST_SKIP() << "no video processor here";
    std::vector<VideoFrame> held;
    for (size_t i = 0; i < d3d11::Nv12Converter::kPictures; ++i) {
        Result<VideoFrame> picture = converter->convert(*texture.Get(), 64, 64, Timestamp{});
        ASSERT_TRUE(picture);
        ASSERT_FALSE(picture->empty()) << "picture " << i;
        held.push_back(std::move(*picture));
    }
    Result<VideoFrame> none = converter->convert(*texture.Get(), 64, 64, Timestamp{});
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->empty());

    held.pop_back();  // its texture goes back to the pool
    Result<VideoFrame> again = converter->convert(*texture.Get(), 64, 64, Timestamp{});
    ASSERT_TRUE(again);
    EXPECT_FALSE(again->empty());

    // A new size starts a new pool, whatever the old one's pictures do.
    Result<VideoFrame> smaller = converter->convert(*texture.Get(), 32, 32, Timestamp{});
    ASSERT_TRUE(smaller);
    EXPECT_FALSE(smaller->empty());
    held.clear();  // given back to a pool given up: let go
}
