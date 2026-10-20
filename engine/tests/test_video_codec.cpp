// Every encoder this build of FFmpeg has, through a decoder and back: what
// goes in comes out, one frame per picture, at once, numbered and linked the
// way MediaSession expects. Encoders whose hardware is not here are skipped.

#include "ratcast/engine/video_codec.h"

#include <gtest/gtest.h>

#include <cctype>
#include <ostream>
#include <random>
#include <string>
#include <vector>

#include "ratcast/engine/pattern_source.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

struct EncoderCase {
    Codec codec;
    std::string name;
};

// How the case is named in test output and ctest: by its encoder.
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

class VideoCodecTest : public ::testing::TestWithParam<EncoderCase> {
protected:
    void SetUp() override {
        config_.codec = GetParam().codec;
        config_.encoder = GetParam().name;
        config_.width = kWidth;
        config_.height = kHeight;
        config_.frame_rate_num = 60;  // live rates are what encoders are tuned for
        config_.bitrate = 2'000'000;
        Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config_);
        if (!encoder)
            GTEST_SKIP() << GetParam().name << " cannot open here: " << to_string(encoder.error());
        encoder_ = std::move(*encoder);

        Result<std::unique_ptr<VideoDecoder>> decoder = open_video_decoder({GetParam().codec, {}});
        if (!decoder) GTEST_SKIP() << "no decoder for this codec: " << to_string(decoder.error());
        decoder_ = std::move(*decoder);
    }

    /// Encode one picture; the frames that came out.
    std::vector<EncodedVideo> encode(const VideoFrame& picture, bool keyframe = false) {
        std::vector<EncodedVideo> out;
        const Result<void> encoded = encoder_->encode(
            picture, keyframe, [&](EncodedVideo&& v) { out.push_back(std::move(v)); });
        EXPECT_TRUE(encoded) << to_string(encoded.error());
        return out;
    }

    /// Decode one frame as a receiver would get it; the pictures that came out.
    std::vector<VideoFrame> decode(EncodedVideo& video) {
        EncodedFrame frame{video.info, Buffer::copy_of(video.payload.bytes())};
        std::vector<VideoFrame> out;
        const Result<void> decoded =
            decoder_->decode(frame, [&](VideoFrame&& p) { out.push_back(std::move(p)); });
        EXPECT_TRUE(decoded) << to_string(decoded.error());
        return out;
    }

    VideoFrame pattern(uint64_t index) {
        const PatternConfig config{encoder_->info().input_format, kWidth, kHeight, 60, 1};
        return draw_pattern(config, index,
                            Timestamp{1s + std::chrono::microseconds(index * 16'667)});
    }

    /// A picture of noise, which no encoder can squeeze below its bitrate.
    VideoFrame noise(uint64_t index) {
        MutableVideoFrame frame =
            MutableVideoFrame::allocate(encoder_->info().input_format, kWidth, kHeight);
        std::mt19937 rng(static_cast<uint32_t>(index));
        for (size_t i = 0; i < plane_count(frame.format()); ++i) {
            for (size_t y = 0; y < plane_rows(frame.format(), i, kHeight); ++y) {
                uint8_t* row = frame.plane(i).data + y * frame.plane(i).stride;
                for (size_t x = 0; x < plane_row_bytes(frame.format(), i, kWidth); ++x)
                    row[x] = uint8_t(rng());
            }
        }
        return std::move(frame).freeze(Timestamp{1s + std::chrono::microseconds(index * 16'667)});
    }

    VideoEncoderConfig config_;
    std::unique_ptr<VideoEncoder> encoder_;
    std::unique_ptr<VideoDecoder> decoder_;
};

}  // namespace

TEST_P(VideoCodecTest, PicturesComeBackOneForOneAndAtOnce) {
    EXPECT_EQ(encoder_->info().name, GetParam().name);
    for (uint64_t i = 0; i < 30; ++i) {
        const VideoFrame picture = pattern(i);
        std::vector<EncodedVideo> frames = encode(picture, i == 15);
        ASSERT_EQ(frames.size(), 1u) << "picture " << i << " was held back";

        EncodedVideo& frame = frames[0];
        EXPECT_EQ(frame.info.number, i);
        EXPECT_EQ(frame.info.capture_time, picture.capture_time());
        const bool keyframe = i == 0 || i == 15;
        EXPECT_EQ(frame.info.keyframe, keyframe) << "picture " << i;
        EXPECT_EQ(frame.info.references,
                  keyframe ? FrameReferences{} : FrameReferences{frame.info.number - 1});
        EXPECT_GE(frame.payload.headroom(), kFrameHeadroom);

        const std::vector<VideoFrame> decoded = decode(frame);
        ASSERT_EQ(decoded.size(), 1u) << "frame " << i << " was held back";
        EXPECT_EQ(decoded[0].width(), kWidth);
        EXPECT_EQ(decoded[0].height(), kHeight);
        EXPECT_EQ(decoded[0].capture_time(), picture.capture_time());
        EXPECT_EQ(read_timestamp(decoded[0]), picture.capture_time()) << "picture " << i;
    }
}

TEST_P(VideoCodecTest, ABitrateChangeTakesEffect) {
    const auto average = [&](uint64_t from, uint64_t to) {
        size_t bytes = 0;
        for (uint64_t i = from; i < to; ++i) {
            for (EncodedVideo& frame : encode(noise(i))) {
                bytes += frame.payload.size();
                EXPECT_EQ(decode(frame).size(), 1u);
            }
        }
        return bytes / (to - from);
    };

    const size_t before = average(0, 60);
    encoder_->set_bitrate(500'000);
    // Settling takes some frames, or a new encoder opened alongside does.
    (void)average(60, 90);
    const size_t after = average(90, 150);
    // A quarter of the rate; ask for less.
    EXPECT_LT(after * 2, before) << "before " << before << " B, after " << after << " B";
}

TEST_P(VideoCodecTest, ABitrateChangeLosesNoPicture) {
    // An encoder that cannot change its rate in flight opens another alongside
    // and switches to it: every picture still comes out, in order, decodable.
    for (uint64_t i = 0; i < 40; ++i) {
        if (i == 5) encoder_->set_bitrate(1'000'000);
        if (i == 6) encoder_->set_bitrate(800'000);  // while the first change opens
        std::vector<EncodedVideo> frames = encode(pattern(i));
        ASSERT_EQ(frames.size(), 1u) << "picture " << i;
        EXPECT_EQ(frames[0].info.number, i);
        EXPECT_EQ(decode(frames[0]).size(), 1u) << "picture " << i;
    }
}

TEST_P(VideoCodecTest, AKeyframeAskedForAsTheBitrateChangesComesOnce) {
    for (uint64_t i = 0; i < 5; ++i) {
        for (EncodedVideo& frame : encode(pattern(i))) decode(frame);
    }
    // As rate control does on frames that expired: a lower bitrate and a
    // keyframe. An encoder that opens another for the bitrate makes the one
    // keyframe it starts with do for both.
    encoder_->set_bitrate(1'000'000);
    int keyframes = 0;
    for (uint64_t i = 5; i < 40; ++i) {
        std::vector<EncodedVideo> frames = encode(pattern(i), i == 5);
        ASSERT_EQ(frames.size(), 1u) << "picture " << i;
        keyframes += frames[0].info.keyframe;
        EXPECT_EQ(decode(frames[0]).size(), 1u) << "picture " << i;
    }
    EXPECT_EQ(keyframes, 1);
}

TEST_P(VideoCodecTest, KeepsGoingAfterAFlush) {
    for (uint64_t i = 0; i < 5; ++i) encode(pattern(i));
    std::vector<EncodedVideo> rest;
    ASSERT_TRUE(encoder_->flush([&](EncodedVideo&& v) { rest.push_back(std::move(v)); }));
    EXPECT_TRUE(rest.empty());  // nothing was held back to begin with

    std::vector<EncodedVideo> next = encode(pattern(5));
    ASSERT_EQ(next.size(), 1u);
    EXPECT_TRUE(next[0].info.keyframe);
    EXPECT_EQ(next[0].info.number, 5u);  // numbering carries on
    EXPECT_EQ(decode(next[0]).size(), 1u);
}

TEST_P(VideoCodecTest, TakesEitherYuvLayout) {
    const PixelFormat other =
        encoder_->info().input_format == PixelFormat::NV12 ? PixelFormat::I420 : PixelFormat::NV12;
    const PatternConfig config{other, kWidth, kHeight, 60, 1};
    const VideoFrame picture = draw_pattern(config, 0, Timestamp{2s});
    std::vector<EncodedVideo> frames = encode(picture);
    ASSERT_EQ(frames.size(), 1u);
    const std::vector<VideoFrame> decoded = decode(frames[0]);
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(read_timestamp(decoded[0]), Timestamp{2s});
}

TEST_P(VideoCodecTest, SignalsTheColorSpace) {
    const ColorSpace color_space{ColorMatrix::BT601, ColorRange::Full};
    VideoEncoderConfig config = config_;
    config.color_space = color_space;
    Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config);
    ASSERT_TRUE(encoder);
    encoder_ = std::move(*encoder);

    VideoFrame picture = pattern(0);
    picture.set_color_space(color_space);
    std::vector<EncodedVideo> frames = encode(picture);
    ASSERT_EQ(frames.size(), 1u);
    const std::vector<VideoFrame> decoded = decode(frames[0]);
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded[0].color_space(), color_space);
}

TEST_P(VideoCodecTest, RefusesPicturesOfAnotherShape) {
    const auto refused = [&](const VideoFrame& picture) {
        const Result<void> encoded = encoder_->encode(picture, false, [](EncodedVideo&&) {});
        return !encoded && encoded.error() == Errc::InvalidArgument;
    };
    const PatternConfig wrong_size{encoder_->info().input_format, kWidth * 2, kHeight, 30, 1};
    EXPECT_TRUE(refused(draw_pattern(wrong_size, 0, Timestamp{})));
    const PatternConfig rgb{PixelFormat::BGRA, kWidth, kHeight, 30, 1};
    EXPECT_TRUE(refused(draw_pattern(rgb, 0, Timestamp{})));

    VideoFrame wrong_colors = pattern(0);
    wrong_colors.set_color_space({ColorMatrix::BT601, ColorRange::Limited});
    EXPECT_TRUE(refused(wrong_colors));
}

INSTANTIATE_TEST_SUITE_P(Encoders, VideoCodecTest, ::testing::ValuesIn(all_encoders()), case_name);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(VideoCodecTest);

TEST(VideoCodec, NeverPicksAGplEncoder) {
    for (Codec codec : {Codec::AV1, Codec::H265, Codec::H264}) {
        for (const std::string& name : video_encoder_candidates(codec)) {
            EXPECT_EQ(name.find("x264"), std::string::npos);
            EXPECT_EQ(name.find("x265"), std::string::npos);
        }
    }
}

TEST(VideoCodec, RefusesConfigsThatMakeNoSense) {
    VideoEncoderConfig config;
    config.width = 320;
    config.height = 240;
    EXPECT_EQ(open_video_encoder([&] {
                  auto c = config;
                  c.width = 0;
                  return c;
              }())
                  .error(),
              Errc::InvalidArgument);
    EXPECT_EQ(open_video_encoder([&] {
                  auto c = config;
                  c.height = 241;
                  return c;
              }())
                  .error(),
              Errc::InvalidArgument);
    EXPECT_EQ(open_video_encoder([&] {
                  auto c = config;
                  c.codec = Codec::Opus;
                  return c;
              }())
                  .error(),
              Errc::InvalidArgument);
    EXPECT_EQ(open_video_encoder([&] {
                  auto c = config;
                  c.encoder = "no-such-encoder";
                  return c;
              }())
                  .error(),
              Errc::Unavailable);
    EXPECT_EQ(open_video_decoder({Codec::Opus, {}}).error(), Errc::InvalidArgument);
}
