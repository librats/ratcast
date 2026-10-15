#include "ratcast/stream/stream_description.h"

#include <gtest/gtest.h>

#include <string>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

StreamDescription camera() {
    StreamDescription d;
    d.id = 1;
    d.codec = Codec::H264;
    d.format = VideoFormat{1280, 720, 30000, 1001};
    d.lifetime = 150ms;
    d.label = "camera";
    return d;
}

}  // namespace

TEST(StreamDescription, KnowsTheKindOfEachCodec) {
    EXPECT_EQ(media_kind_of(Codec::AV1), MediaKind::Video);
    EXPECT_EQ(media_kind_of(Codec::Opus), MediaKind::Audio);
    EXPECT_EQ(media_kind_of(static_cast<Codec>(250)), std::nullopt);
}

TEST(StreamDescription, AcceptsAWellFormedStream) {
    EXPECT_TRUE(is_valid(camera()));
    EXPECT_EQ(camera().kind(), MediaKind::Video);
    ASSERT_NE(camera().video(), nullptr);
    EXPECT_EQ(camera().audio(), nullptr);
}

TEST(StreamDescription, RefusesWhatCannotBeAnnounced) {
    const auto refuse = [](auto change) {
        StreamDescription d = camera();
        change(d);
        EXPECT_FALSE(is_valid(d));
    };
    refuse([](StreamDescription& d) { d.lifetime = 0ms; });
    refuse([](StreamDescription& d) { d.lifetime = StreamDescription::kMaxLifetime + 1us; });
    refuse([](StreamDescription& d) {
        d.label = std::string(StreamDescription::kMaxLabelSize + 1, 'x');
    });
    refuse([](StreamDescription& d) { d.codec = Codec::Opus; });  // audio codec, video format
    refuse([](StreamDescription& d) { d.format = VideoFormat{0, 720, 30, 1}; });
    refuse([](StreamDescription& d) { d.format = VideoFormat{1280, 720, 30, 0}; });
    refuse([](StreamDescription& d) {
        d.format = VideoFormat{StreamDescription::kMaxDimension + 1, 720, 30, 1};
    });
    refuse([](StreamDescription& d) {
        d.codec = Codec::Opus;
        d.format = AudioFormat{48000, 0};
    });
}

TEST(StreamDescription, AnUnknownCodecMayCarryEitherKind) {
    StreamDescription d = camera();
    d.codec = static_cast<Codec>(250);
    EXPECT_TRUE(is_valid(d));
    d.format = AudioFormat{48000, 2};
    EXPECT_TRUE(is_valid(d));
}

TEST(StreamDescription, ComparesCodecConfigByContent) {
    StreamDescription a = camera();
    StreamDescription b = camera();
    const uint8_t config[] = {1, 2, 3};
    a.codec_config = Buffer::copy_of(config);
    b.codec_config = Buffer::copy_of(config);
    EXPECT_EQ(a, b);
    b.label = "other";
    EXPECT_NE(a, b);
}
