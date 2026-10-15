#include "ratcast/engine/video_frame.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr PixelFormat kFormats[] = {PixelFormat::I420, PixelFormat::NV12, PixelFormat::BGRA};

}  // namespace

TEST(VideoFrame, PlaneGeometryRoundsChromaUp) {
    EXPECT_EQ(plane_row_bytes(PixelFormat::I420, 0, 101), 101u);
    EXPECT_EQ(plane_row_bytes(PixelFormat::I420, 1, 101), 51u);
    EXPECT_EQ(plane_row_bytes(PixelFormat::NV12, 1, 101), 102u);
    EXPECT_EQ(plane_row_bytes(PixelFormat::BGRA, 0, 101), 404u);
    EXPECT_EQ(plane_rows(PixelFormat::NV12, 1, 75), 38u);
    EXPECT_EQ(plane_rows(PixelFormat::BGRA, 0, 75), 75u);
}

TEST(VideoFrame, AllocatedPlanesAreAlignedForSimd) {
    for (PixelFormat format : kFormats) {
        const MutableVideoFrame frame = MutableVideoFrame::allocate(format, 101, 75);
        for (size_t i = 0; i < plane_count(format); ++i) {
            const auto& plane = frame.plane(i);
            EXPECT_EQ(reinterpret_cast<uintptr_t>(plane.data) % 64, 0u);
            EXPECT_EQ(plane.stride % 64, 0u);
            EXPECT_GE(plane.stride, plane_row_bytes(format, i, 101));
        }
    }
}

TEST(VideoFrame, FreezingKeepsThePixelsInPlace) {
    MutableVideoFrame frame = MutableVideoFrame::allocate(PixelFormat::NV12, 64, 32);
    for (size_t i = 0; i < 2; ++i) {
        for (size_t y = 0; y < plane_rows(PixelFormat::NV12, i, 32); ++y) {
            uint8_t* row = frame.plane(i).data + y * frame.plane(i).stride;
            for (size_t x = 0; x < 64; ++x) row[x] = uint8_t(x + y + i);
        }
    }
    const uint8_t* luma = frame.plane(0).data;

    const VideoFrame picture = std::move(frame).freeze(Timestamp{3s});
    EXPECT_EQ(picture.plane(0).data, luma);
    EXPECT_EQ(picture.capture_time(), Timestamp{3s});
    EXPECT_EQ(picture.plane(1).data[5 * picture.plane(1).stride + 7], uint8_t(7 + 5 + 1));

    const VideoFrame copy = picture;  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(copy.plane(0).data, luma);
    EXPECT_EQ(copy.width(), 64u);
    EXPECT_EQ(copy.plane_count(), 2u);
}

TEST(VideoFrame, WrapsMemoryItDoesNotOwnAndKeepsItAlive) {
    auto pixels = std::make_shared<std::array<uint8_t, 64 * 64 * 4>>();
    std::weak_ptr<void> watch = pixels;
    const VideoFrame::Plane plane{pixels->data(), 64 * 4};

    VideoFrame picture(PixelFormat::BGRA, 64, 64, {&plane, 1}, std::move(pixels), Timestamp{});
    EXPECT_FALSE(watch.expired());
    VideoFrame copy = picture;
    picture = VideoFrame{};
    EXPECT_FALSE(watch.expired());
    copy = VideoFrame{};
    EXPECT_TRUE(watch.expired());
}

TEST(VideoFrame, DefaultIsEmpty) { EXPECT_TRUE(VideoFrame{}.empty()); }
