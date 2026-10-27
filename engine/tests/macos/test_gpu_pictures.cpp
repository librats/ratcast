// Pictures on IOSurfaces, as a screen capture hands them out when asked for
// GPU pictures: read where they are with no copy, copied out as they are,
// converted; only NV12 of even sizes on a surface taken. And every encoder
// given them: VideoToolbox takes the surfaces as they are, the others read
// them in place; either way what goes in comes out. Encoders skip where their
// hardware is not here (CI's runner may have none).

#include "macos/pixel_buffer.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cstring>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <vector>

#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/video_codec.h"
#include "ratcast/engine/video_convert.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;

struct Release {
    void operator()(CVPixelBufferRef pixels) const noexcept { CVPixelBufferRelease(pixels); }
};
using PixelsPtr = std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, Release>;

/// A new pixel buffer of `type`, on an IOSurface or in plain memory.
PixelsPtr make_pixels(OSType type, uint32_t width, uint32_t height, bool surface = true) {
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (surface) {
        CFDictionaryRef none =
            CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, none);
        CFRelease(none);
    }
    CVPixelBufferRef pixels = nullptr;
    CVPixelBufferCreate(kCFAllocatorDefault, width, height, type, attributes, &pixels);
    CFRelease(attributes);
    return PixelsPtr(pixels);
}

/// `nv12` copied onto an IOSurface, as a GPU picture of the capture's.
VideoFrame on_surface(const VideoFrame& nv12) {
    const OSType type = nv12.color_space().range == ColorRange::Full
                            ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                            : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    const PixelsPtr pixels = make_pixels(type, nv12.width(), nv12.height());
    if (!pixels || CVPixelBufferLockBaseAddress(pixels.get(), 0) != kCVReturnSuccess) return {};
    for (size_t i = 0; i < nv12.plane_count(); ++i) {
        auto* to = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels.get(), i));
        const size_t to_stride = CVPixelBufferGetBytesPerRowOfPlane(pixels.get(), i);
        const size_t bytes = plane_row_bytes(PixelFormat::NV12, i, nv12.width());
        for (size_t y = 0; y < plane_rows(PixelFormat::NV12, i, nv12.height()); ++y)
            std::memcpy(to + y * to_stride, nv12.plane(i).data + y * nv12.plane(i).stride, bytes);
    }
    CVPixelBufferUnlockBaseAddress(pixels.get(), 0);
    return macos::gpu_frame_of(pixels.get(), nv12.capture_time(), nv12.color_space());
}

/// Whether two pictures in memory hold the same samples.
::testing::AssertionResult same_samples(const VideoFrame& a, const VideoFrame& b) {
    if (a.format() != b.format() || a.width() != b.width() || a.height() != b.height())
        return ::testing::AssertionFailure() << "of another format or size";
    for (size_t i = 0; i < a.plane_count(); ++i) {
        for (size_t y = 0; y < plane_rows(a.format(), i, a.height()); ++y) {
            if (std::memcmp(a.plane(i).data + y * a.plane(i).stride,
                            b.plane(i).data + y * b.plane(i).stride,
                            plane_row_bytes(a.format(), i, a.width())) != 0) {
                return ::testing::AssertionFailure() << "plane " << i << " differs in row " << y;
            }
        }
    }
    return ::testing::AssertionSuccess();
}

Timestamp time_of(uint64_t index) {
    return Timestamp{1s + std::chrono::microseconds(index * 16'667)};
}

VideoFrame pattern(uint64_t index, PixelFormat format = PixelFormat::NV12) {
    return draw_pattern({format, kWidth, kHeight}, index, time_of(index));
}

}  // namespace

TEST(MacGpuPictureTest, IsReadWhereItIs) {
    const VideoFrame nv12 = pattern(3);
    const VideoFrame gpu = on_surface(nv12);
    ASSERT_FALSE(gpu.empty());
    ASSERT_TRUE(gpu.gpu());
    EXPECT_EQ(gpu.gpu()->device()->api(), GpuApi::CoreVideo);
    EXPECT_EQ(gpu.gpu()->device(), macos::core_video_device());
    EXPECT_EQ(gpu.format(), PixelFormat::NV12);
    EXPECT_EQ(gpu.width(), kWidth);
    EXPECT_EQ(gpu.height(), kHeight);
    EXPECT_EQ(gpu.plane(0).data, nullptr);
    CVPixelBufferRef pixels = macos::pixel_buffer_of(gpu);
    ASSERT_NE(pixels, nullptr);
    EXPECT_EQ(macos::pixel_buffer_of(nv12), nullptr);

    const Result<VideoFrame> read = download_frame(gpu);
    ASSERT_TRUE(read);
    EXPECT_FALSE(read->gpu());
    // The surface's own memory, locked while the frame lives.
    EXPECT_EQ(read->plane(0).data, CVPixelBufferGetBaseAddressOfPlane(pixels, 0));
    EXPECT_EQ(read->capture_time(), nv12.capture_time());
    EXPECT_EQ(read->color_space(), nv12.color_space());
    EXPECT_TRUE(same_samples(*read, nv12));
    EXPECT_EQ(read_timestamp(*read), time_of(3));
}

TEST(MacGpuPictureTest, IsCopiedOutAsItIs) {
    const VideoFrame nv12 = pattern(4);
    const VideoFrame gpu = on_surface(nv12);
    ASSERT_TRUE(gpu.gpu());
    MutableVideoFrame copy = MutableVideoFrame::allocate(PixelFormat::NV12, kWidth, kHeight);
    ASSERT_TRUE(gpu.gpu()->read(copy));
    EXPECT_TRUE(same_samples(std::move(copy).freeze(nv12.capture_time()), nv12));

    MutableVideoFrame other = MutableVideoFrame::allocate(PixelFormat::NV12, kWidth / 2, kHeight);
    EXPECT_FALSE(gpu.gpu()->read(other)) << "of another size";
}

TEST(MacGpuPictureTest, IsConvertedAsThePictureInMemoryIs) {
    const VideoFrame nv12 = pattern(5);
    const VideoFrame gpu = on_surface(nv12);
    ASSERT_TRUE(gpu.gpu());
    const Result<VideoFrame> from_gpu = convert_frame(gpu, PixelFormat::I420);
    const Result<VideoFrame> from_memory = convert_frame(nv12, PixelFormat::I420);
    ASSERT_TRUE(from_gpu);
    ASSERT_TRUE(from_memory);
    EXPECT_TRUE(same_samples(*from_gpu, *from_memory));
    EXPECT_EQ(read_timestamp(*from_gpu), time_of(5));
}

TEST(MacGpuPictureTest, OnlyNv12OfEvenSizesOnASurface) {
    const auto gpu_frame = [](const PixelsPtr& pixels) {
        return pixels ? macos::gpu_frame_of(pixels.get(), Timestamp{1s}, {}) : VideoFrame{};
    };
    EXPECT_FALSE(
        gpu_frame(make_pixels(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 64, 48)).empty());
    EXPECT_TRUE(gpu_frame(make_pixels(kCVPixelFormatType_32BGRA, 64, 48)).empty());
    EXPECT_TRUE(
        gpu_frame(make_pixels(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, 64, 48, false))
            .empty())
        << "in plain memory";
    EXPECT_TRUE(
        gpu_frame(make_pixels(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, 63, 48)).empty())
        << "of an odd width";
}

namespace {

struct EncoderCase {
    Codec codec;
    std::string name;
};

void PrintTo(const EncoderCase& c, std::ostream* out) { *out << c.name; }

std::vector<EncoderCase> all_encoders() {
    std::vector<EncoderCase> cases;
    for (Codec codec : {Codec::AV1, Codec::H265, Codec::H264}) {
        for (std::string& name : video_encoder_candidates(codec))
            cases.push_back({codec, std::move(name)});
    }
    return cases;
}

std::string case_name(const ::testing::TestParamInfo<EncoderCase>& info) {
    std::string name = info.param.name;
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    return name;
}

class MacGpuEncodingTest : public ::testing::TestWithParam<EncoderCase> {
protected:
    void SetUp() override {
        VideoEncoderConfig config;
        config.codec = GetParam().codec;
        config.encoder = GetParam().name;
        config.width = kWidth;
        config.height = kHeight;
        config.bitrate = 2'000'000;
        config.device = macos::core_video_device();
        Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config);
        if (!encoder)
            GTEST_SKIP() << GetParam().name << " cannot open here: " << to_string(encoder.error());
        encoder_ = std::move(*encoder);
        Result<std::unique_ptr<VideoDecoder>> decoder = open_video_decoder({GetParam().codec, {}});
        if (!decoder) GTEST_SKIP() << "no decoder for this codec";
        decoder_ = std::move(*decoder);
    }

    /// Encode `picture` and decode what comes out: its stamp, read back.
    std::optional<Timestamp> round_trip(const VideoFrame& picture, bool keyframe = false) {
        std::vector<EncodedVideo> frames;
        const Result<void> encoded = encoder_->encode(
            picture, keyframe, [&](EncodedVideo&& v) { frames.push_back(std::move(v)); });
        EXPECT_TRUE(encoded) << to_string(encoded.error());
        if (frames.size() != 1) return std::nullopt;
        EXPECT_EQ(frames[0].info.capture_time, picture.capture_time());
        if (keyframe) {
            EXPECT_TRUE(frames[0].info.keyframe);
        }
        const EncodedFrame frame{frames[0].info, Buffer::copy_of(frames[0].payload.bytes())};
        std::optional<Timestamp> stamp;
        EXPECT_TRUE(decoder_->decode(frame, [&](VideoFrame&& p) { stamp = read_timestamp(p); }));
        return stamp;
    }

    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<VideoDecoder> decoder_;
};

}  // namespace

TEST_P(MacGpuEncodingTest, GpuPicturesComeBack) {
    // VideoToolbox takes the surfaces as they are; nothing else does.
    EXPECT_EQ(encoder_->info().gpu_input,
              GetParam().name.find("videotoolbox") != std::string::npos);
    for (uint64_t i = 0; i < 20; ++i) {
        EXPECT_EQ(round_trip(on_surface(pattern(i)), i == 10), time_of(i)) << "picture " << i;
    }
}

TEST_P(MacGpuEncodingTest, PicturesInMemoryComeBackToo) {
    EXPECT_EQ(round_trip(pattern(0)), time_of(0));
    EXPECT_EQ(round_trip(pattern(1, PixelFormat::I420)), time_of(1));
    EXPECT_EQ(round_trip(on_surface(pattern(2))), time_of(2));
}

TEST_P(MacGpuEncodingTest, KeepsGoingAfterAFlushAndABitrateChange) {
    for (uint64_t i = 0; i < 5; ++i) EXPECT_EQ(round_trip(on_surface(pattern(i))), time_of(i));
    ASSERT_TRUE(encoder_->flush([](EncodedVideo&&) {}));
    EXPECT_EQ(round_trip(on_surface(pattern(5))), time_of(5));
    encoder_->set_bitrate(1'000'000);
    for (uint64_t i = 6; i < 30; ++i) {
        EXPECT_EQ(round_trip(on_surface(pattern(i))), time_of(i)) << "picture " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(Encoders, MacGpuEncodingTest, ::testing::ValuesIn(all_encoders()),
                         case_name);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(MacGpuEncodingTest);
