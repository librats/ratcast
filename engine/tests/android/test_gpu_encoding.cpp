// Pictures in AHardwareBuffers, as the screen hands them out when asked for
// GPU pictures: read by the CPU where they are, converted. And every encoder
// given them: MediaCodec's draws them into its input surface, the others read
// them; either way what goes in comes out, in the colours it went in with —
// the encoder turns the RGB drawn into YUV itself. Pictures in memory go into
// an encoder opened for the GPU as well, drawn from their planes.

#include "android/hardware_picture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/video_codec.h"
#include "ratcast/engine/video_convert.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;

/// `yuv` (4:2:0 in memory) as RGBA, as a shader turns it into colour.
VideoFrame to_rgba(const VideoFrame& yuv) {
    const YuvToRgb k = rgb_from_yuv(yuv.color_space());
    MutableVideoFrame out =
        MutableVideoFrame::allocate(PixelFormat::RGBA, yuv.width(), yuv.height());
    const bool nv12 = yuv.format() == PixelFormat::NV12;
    for (uint32_t y = 0; y < yuv.height(); ++y) {
        for (uint32_t x = 0; x < yuv.width(); ++x) {
            const float luma = yuv.plane(0).data[y * yuv.plane(0).stride + x] / 255.f;
            const uint8_t* chroma = yuv.plane(1).data + (y / 2) * yuv.plane(1).stride;
            const float u = (nv12 ? chroma[x / 2 * 2] : chroma[x / 2]) / 255.f;
            const float v = (nv12 ? chroma[x / 2 * 2 + 1]
                                  : yuv.plane(2).data[(y / 2) * yuv.plane(2).stride + x / 2]) /
                            255.f;
            uint8_t* to = out.plane(0).data + y * out.plane(0).stride + x * 4;
            for (int c = 0; c < 3; ++c) {
                const float value = k[c][0] * luma + k[c][1] * u + k[c][2] * v + k[c][3];
                to[c] = uint8_t(std::lround(std::clamp(value, 0.f, 1.f) * 255));
            }
            to[3] = 255;
        }
    }
    return std::move(out).freeze(yuv.capture_time(), yuv.color_space());
}

/// A picture of one colour, `r`, `g`, `b`, in `color_space` once YUV.
VideoFrame flat(uint8_t r, uint8_t g, uint8_t b, ColorSpace color_space, Timestamp at) {
    MutableVideoFrame out = MutableVideoFrame::allocate(PixelFormat::RGBA, kWidth, kHeight);
    for (uint32_t y = 0; y < kHeight; ++y) {
        uint8_t* row = out.plane(0).data + y * out.plane(0).stride;
        for (uint32_t x = 0; x < kWidth; ++x) {
            row[x * 4] = r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = b;
            row[x * 4 + 3] = 255;
        }
    }
    return std::move(out).freeze(at, color_space);
}

/// The Y, U and V at the centre of a 4:2:0 picture in memory.
std::array<int, 3> centre_of(const VideoFrame& yuv) {
    const uint32_t x = yuv.width() / 2;
    const uint32_t y = yuv.height() / 2;
    const uint8_t* chroma = yuv.plane(1).data + (y / 2) * yuv.plane(1).stride;
    if (yuv.format() == PixelFormat::NV12)
        return {yuv.plane(0).data[y * yuv.plane(0).stride + x], chroma[x / 2 * 2],
                chroma[x / 2 * 2 + 1]};
    return {yuv.plane(0).data[y * yuv.plane(0).stride + x], chroma[x / 2],
            yuv.plane(2).data[(y / 2) * yuv.plane(2).stride + x / 2]};
}

VideoFrame on_gpu(const VideoFrame& rgba) {
    Result<VideoFrame> uploaded = android::upload(rgba);
    EXPECT_TRUE(uploaded) << (uploaded ? "" : to_string(uploaded.error()));
    return uploaded ? std::move(*uploaded) : VideoFrame{};
}

}  // namespace

TEST(HardwareBufferTest, IsReadWhereItIsAndConverted) {
    const VideoFrame rgba =
        to_rgba(draw_pattern({PixelFormat::NV12, kWidth, kHeight}, 3, Timestamp{2s}));
    const VideoFrame picture = on_gpu(rgba);
    ASSERT_FALSE(picture.empty());
    EXPECT_EQ(picture.format(), PixelFormat::RGBA);
    EXPECT_EQ(picture.gpu()->device(), android::hardware_buffer_device());
    EXPECT_NE(android::hardware_buffer_of(picture), nullptr);
    EXPECT_EQ(android::hardware_buffer_of(rgba), nullptr);

    Result<VideoFrame> read = download_frame(picture);
    ASSERT_TRUE(read);
    ASSERT_EQ(read->format(), PixelFormat::RGBA);
    for (uint32_t y = 0; y < kHeight; ++y) {
        ASSERT_EQ(std::memcmp(read->plane(0).data + y * read->plane(0).stride,
                              rgba.plane(0).data + y * rgba.plane(0).stride, kWidth * 4),
                  0)
            << "row " << y;
    }
    Result<VideoFrame> nv12 = convert_frame(picture, PixelFormat::NV12, picture.color_space());
    ASSERT_TRUE(nv12);
    EXPECT_EQ(read_timestamp(*nv12), Timestamp{2s});
    EXPECT_FALSE(nv12->gpu());
}

namespace {

struct EncoderCase {
    Codec codec;
    std::string name;
};

void PrintTo(const EncoderCase& c, std::ostream* out) { *out << c.name; }

std::vector<EncoderCase> all_encoders() {
    std::vector<EncoderCase> cases;
    for (Codec codec : {Codec::H265, Codec::H264}) {
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

class GpuEncodingTest : public ::testing::TestWithParam<EncoderCase> {
protected:
    void SetUp() override { open({}); }

    void open(ColorSpace color_space) {
        VideoEncoderConfig config;
        config.codec = GetParam().codec;
        config.encoder = GetParam().name;
        config.width = kWidth;
        config.height = kHeight;
        config.bitrate = 2'000'000;
        config.color_space = color_space;
        config.device = android::hardware_buffer_device();
        Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config);
        if (!encoder)
            GTEST_SKIP() << GetParam().name << " cannot open here: " << to_string(encoder.error());
        encoder_ = std::move(*encoder);
        Result<std::unique_ptr<VideoDecoder>> decoder = open_video_decoder({GetParam().codec, {}});
        if (!decoder) GTEST_SKIP() << "no decoder for this codec";
        decoder_ = std::move(*decoder);
    }

    static Timestamp time_of(uint64_t index) {
        return Timestamp{1s + std::chrono::microseconds(index * 16'667)};
    }

    VideoFrame pattern(uint64_t index, PixelFormat format = PixelFormat::NV12) {
        return draw_pattern({format, kWidth, kHeight}, index, time_of(index));
    }

    /// Encode `picture` and decode what comes out.
    std::optional<VideoFrame> round_trip(const VideoFrame& picture, bool keyframe = false) {
        std::vector<EncodedVideo> frames;
        const Result<void> encoded = encoder_->encode(
            picture, keyframe, [&](EncodedVideo&& v) { frames.push_back(std::move(v)); });
        EXPECT_TRUE(encoded) << to_string(encoded.error());
        if (frames.size() != 1) return std::nullopt;
        EXPECT_EQ(frames[0].info.capture_time, picture.capture_time());
        if (keyframe) EXPECT_TRUE(frames[0].info.keyframe);
        const EncodedFrame frame{frames[0].info, Buffer::copy_of(frames[0].payload.bytes())};
        std::optional<VideoFrame> decoded;
        EXPECT_TRUE(decoder_->decode(frame, [&](VideoFrame&& p) { decoded = std::move(p); }));
        return decoded;
    }

    std::optional<Timestamp> stamp_of(const VideoFrame& picture, bool keyframe = false) {
        const std::optional<VideoFrame> decoded = round_trip(picture, keyframe);
        return decoded ? read_timestamp(*decoded) : std::nullopt;
    }

    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<VideoDecoder> decoder_;
};

}  // namespace

TEST_P(GpuEncodingTest, GpuPicturesComeBack) {
    // MediaCodec's encoders draw the buffers where they are.
    RecordProperty("gpu_input", encoder_->info().gpu_input ? "yes" : "no");
    EXPECT_EQ(encoder_->info().gpu_input, GetParam().name.ends_with("_mediacodec"));
    for (uint64_t i = 0; i < 20; ++i) {
        EXPECT_EQ(stamp_of(on_gpu(to_rgba(pattern(i))), i == 10), time_of(i)) << "picture " << i;
    }
}

TEST_P(GpuEncodingTest, PicturesInMemoryComeBackToo) {
    EXPECT_EQ(stamp_of(pattern(0)), time_of(0));
    EXPECT_EQ(stamp_of(pattern(1, PixelFormat::I420)), time_of(1));
    EXPECT_EQ(stamp_of(on_gpu(to_rgba(pattern(2)))), time_of(2));
}

TEST_P(GpuEncodingTest, KeepsGoingAfterAFlushAndABitrateChange) {
    for (uint64_t i = 0; i < 5; ++i) EXPECT_EQ(stamp_of(on_gpu(to_rgba(pattern(i)))), time_of(i));
    ASSERT_TRUE(encoder_->flush([](EncodedVideo&&) {}));
    EXPECT_EQ(stamp_of(on_gpu(to_rgba(pattern(5)))), time_of(5));
    encoder_->set_bitrate(1'000'000);
    for (uint64_t i = 6; i < 30; ++i) {
        EXPECT_EQ(stamp_of(on_gpu(to_rgba(pattern(i)))), time_of(i)) << "picture " << i;
    }
}

TEST_P(GpuEncodingTest, ColoursComeOutAsTheStreamSays) {
    // The encoder's own RGB-to-YUV against convert_frame()'s in the colour
    // space the stream signals, a few steps apart at most: the one asked for
    // or, drawn into a surface, BT.601 at limited range — what Qualcomm's
    // encoders turn RGB into and say they do, whatever is asked.
    for (const ColorSpace space : {ColorSpace{ColorMatrix::BT709, ColorRange::Limited},
                                   ColorSpace{ColorMatrix::BT601, ColorRange::Limited},
                                   ColorSpace{ColorMatrix::BT709, ColorRange::Full},
                                   ColorSpace{ColorMatrix::BT601, ColorRange::Full}}) {
        open(space);
        if (IsSkipped()) return;
        const ColorSpace signalled = encoder_->info().gpu_input
                                         ? ColorSpace{ColorMatrix::BT601, ColorRange::Limited}
                                         : space;
        const auto name = [&] {
            return std::string(space.matrix == ColorMatrix::BT709 ? "BT.709" : "BT.601") +
                   (space.range == ColorRange::Full ? " full" : " limited");
        };
        uint64_t index = 0;
        for (const auto [r, g, b] :
             {std::array<uint8_t, 3>{200, 60, 40}, {40, 180, 90}, {70, 80, 210}, {128, 128, 128}}) {
            Result<VideoFrame> expected =
                convert_frame(flat(r, g, b, space, Timestamp{}), PixelFormat::NV12, signalled);
            ASSERT_TRUE(expected);
            // A few pictures in, the colour settled.
            std::optional<VideoFrame> decoded;
            for (int i = 0; i < 3; ++i)
                decoded = round_trip(on_gpu(flat(r, g, b, space, time_of(index++))), i == 0);
            ASSERT_TRUE(decoded) << name();
            EXPECT_EQ(decoded->color_space(), signalled) << name();
            const std::array<int, 3> want = centre_of(*expected);
            const std::array<int, 3> got = centre_of(*decoded);
            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(got[c], want[c], 4) << name() << ", RGB " << int(r) << " " << int(g)
                                                << " " << int(b) << ", component " << c;
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Encoders, GpuEncodingTest, ::testing::ValuesIn(all_encoders()), case_name);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(GpuEncodingTest);

// A decoder asked for GPU pictures renders into AHardwareBuffers: pictures a
// view draws where they are, which anything else still reads.
TEST(GpuDecodingTest, PicturesStayInGpuMemoryAndAreRead) {
    for (const Codec codec : {Codec::H265, Codec::H264}) {
        VideoEncoderConfig config;
        config.codec = codec;
        config.width = kWidth;
        config.height = kHeight;
        config.bitrate = 2'000'000;
        Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config);
        Result<std::unique_ptr<VideoDecoder>> decoder =
            open_video_decoder({.codec = codec, .codec_config = {}, .gpu = true});
        if (!encoder || !decoder) continue;
        for (uint64_t i = 0; i < 10; ++i) {
            const Timestamp at{1s + std::chrono::milliseconds(i * 17)};
            std::vector<EncodedVideo> frames;
            ASSERT_TRUE((*encoder)->encode(
                draw_pattern({PixelFormat::NV12, kWidth, kHeight}, i, at), i == 0,
                [&](EncodedVideo&& v) { frames.push_back(std::move(v)); }));
            ASSERT_EQ(frames.size(), 1u);
            const EncodedFrame frame{frames[0].info, Buffer::copy_of(frames[0].payload.bytes())};
            std::vector<VideoFrame> pictures;
            ASSERT_TRUE((*decoder)->decode(
                frame, [&](VideoFrame&& p) { pictures.push_back(std::move(p)); }));
            ASSERT_EQ(pictures.size(), 1u) << to_string(codec) << ", picture " << i;
            const VideoFrame& picture = pictures[0];
            EXPECT_NE(android::hardware_buffer_of(picture), nullptr);
            EXPECT_EQ(picture.format(), PixelFormat::NV12);
            EXPECT_EQ(picture.width(), kWidth);
            EXPECT_EQ(picture.height(), kHeight);
            EXPECT_EQ(picture.capture_time(), at);
            Result<VideoFrame> read = download_frame(picture);
            ASSERT_TRUE(read);
            EXPECT_FALSE(read->gpu());
            EXPECT_EQ(read_timestamp(*read), at) << to_string(codec) << ", picture " << i;
        }
    }
}
