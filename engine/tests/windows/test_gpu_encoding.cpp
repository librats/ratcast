// Every encoder, given pictures in Direct3D 11's memory: a hardware encoder
// that takes the device's textures reads them where they are, the others
// download them; either way what goes in comes out. Pictures in memory go into
// an encoder opened for the GPU as well. Skipped where an encoder's hardware,
// or the GPU's video API, is not here.

#include <gtest/gtest.h>

#include <cctype>
#include <ostream>
#include <string>
#include <vector>

#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/video_codec.h"
#include "windows/d3d11.h"

using namespace ratcast;
using namespace std::chrono_literals;

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

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;

class GpuEncodingTest : public ::testing::TestWithParam<EncoderCase> {
protected:
    void SetUp() override {
        Result<std::shared_ptr<d3d11::Device>> device = d3d11::Device::create();
        if (!device || !(*device)->video()) GTEST_SKIP() << "no GPU with the video API here";
        device_ = std::move(*device);

        VideoEncoderConfig config;
        config.codec = GetParam().codec;
        config.encoder = GetParam().name;
        config.width = kWidth;
        config.height = kHeight;
        config.bitrate = 2'000'000;
        config.device = device_;
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

    VideoFrame on_gpu(const VideoFrame& picture) {
        Result<VideoFrame> uploaded = d3d11::upload(device_, picture);
        EXPECT_TRUE(uploaded);
        return uploaded ? std::move(*uploaded) : VideoFrame{};
    }

    /// Encode `picture` and decode what comes out: its stamp, read back.
    std::optional<Timestamp> round_trip(const VideoFrame& picture, bool keyframe = false) {
        std::vector<EncodedVideo> frames;
        const Result<void> encoded = encoder_->encode(
            picture, keyframe, [&](EncodedVideo&& v) { frames.push_back(std::move(v)); });
        EXPECT_TRUE(encoded) << to_string(encoded.error());
        if (frames.size() != 1) return std::nullopt;
        EXPECT_EQ(frames[0].info.capture_time, picture.capture_time());
        if (keyframe) EXPECT_TRUE(frames[0].info.keyframe);
        const EncodedFrame frame{frames[0].info, Buffer::copy_of(frames[0].payload.bytes())};
        std::optional<Timestamp> stamp;
        EXPECT_TRUE(decoder_->decode(frame, [&](VideoFrame&& p) { stamp = read_timestamp(p); }));
        return stamp;
    }

    std::shared_ptr<d3d11::Device> device_;
    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<VideoDecoder> decoder_;
};

}  // namespace

TEST_P(GpuEncodingTest, GpuPicturesComeBack) {
    // Hardware encoders that take D3D11 frames read the pictures in place.
    RecordProperty("gpu_input", encoder_->info().gpu_input ? "yes" : "no");
    if (GetParam().name.find("nvenc") != std::string::npos) {
        EXPECT_TRUE(encoder_->info().gpu_input);
    }
    if (!encoder_->info().hardware) EXPECT_FALSE(encoder_->info().gpu_input);
    for (uint64_t i = 0; i < 20; ++i) {
        EXPECT_EQ(round_trip(on_gpu(pattern(i)), i == 10), time_of(i)) << "picture " << i;
    }
}

TEST_P(GpuEncodingTest, PicturesInMemoryComeBackToo) {
    EXPECT_EQ(round_trip(pattern(0)), time_of(0));
    EXPECT_EQ(round_trip(pattern(1, PixelFormat::I420)), time_of(1));
    EXPECT_EQ(round_trip(on_gpu(pattern(2))), time_of(2));
}

TEST_P(GpuEncodingTest, KeepsGoingAfterAFlushAndABitrateChange) {
    for (uint64_t i = 0; i < 5; ++i) EXPECT_EQ(round_trip(on_gpu(pattern(i))), time_of(i));
    ASSERT_TRUE(encoder_->flush([](EncodedVideo&&) {}));
    EXPECT_EQ(round_trip(on_gpu(pattern(5))), time_of(5));
    encoder_->set_bitrate(1'000'000);
    for (uint64_t i = 6; i < 30; ++i) {
        EXPECT_EQ(round_trip(on_gpu(pattern(i))), time_of(i)) << "picture " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(Encoders, GpuEncodingTest, ::testing::ValuesIn(all_encoders()), case_name);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(GpuEncodingTest);
