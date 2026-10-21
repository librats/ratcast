#include "ratcast/engine/video_convert.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include "ratcast/engine/pattern_source.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr ColorSpace kColorSpaces[] = {
    {ColorMatrix::BT709, ColorRange::Limited},
    {ColorMatrix::BT709, ColorRange::Full},
    {ColorMatrix::BT601, ColorRange::Limited},
    {ColorMatrix::BT601, ColorRange::Full},
};

VideoFrame random_bgra(uint32_t width, uint32_t height, uint32_t seed) {
    MutableVideoFrame frame = MutableVideoFrame::allocate(PixelFormat::BGRA, width, height);
    std::mt19937 rng(seed);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = frame.plane(0).data + y * frame.plane(0).stride;
        for (uint32_t x = 0; x < width * 4; ++x) row[x] = static_cast<uint8_t>(rng());
    }
    return std::move(frame).freeze(Timestamp{7s});
}

VideoFrame solid_bgra(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b) {
    MutableVideoFrame frame = MutableVideoFrame::allocate(PixelFormat::BGRA, width, height);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = frame.plane(0).data + y * frame.plane(0).stride;
        for (uint32_t x = 0; x < width; ++x) {
            row[4 * x] = b;
            row[4 * x + 1] = g;
            row[4 * x + 2] = r;
            row[4 * x + 3] = 0xFF;
        }
    }
    return std::move(frame).freeze(Timestamp{});
}

/// Y'CbCr of one RGB colour by the book, in floating point.
struct Reference {
    double y, cb, cr;
};

Reference reference(ColorSpace color_space, double r, double g, double b) {
    const bool bt709 = color_space.matrix == ColorMatrix::BT709;
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double luma = (kr * r + (1 - kr - kb) * g + kb * b) / 255;
    const double cb = (b / 255 - luma) / (2 * (1 - kb));
    const double cr = (r / 255 - luma) / (2 * (1 - kr));
    if (color_space.range == ColorRange::Full) {
        return {luma * 255, std::min(128 + cb * 255, 255.0), std::min(128 + cr * 255, 255.0)};
    }
    return {16 + luma * 219, 128 + cb * 224, 128 + cr * 224};
}

uint8_t pixel(const VideoFrame& frame, size_t plane, uint32_t x, uint32_t y) {
    return frame.plane(plane).data[y * frame.plane(plane).stride + x];
}

/// The largest difference between `yuv` and the reference conversion of
/// `bgra`, with chroma from the mean colour of each 2 x 2 block.
double max_error(const VideoFrame& bgra, const VideoFrame& yuv) {
    const ColorSpace cs = yuv.color_space();
    const auto rgb = [&](uint32_t x, uint32_t y, int channel) {
        return double(pixel(bgra, 0, 4 * std::min(x, bgra.width() - 1) + channel,
                            std::min(y, bgra.height() - 1)));
    };
    double worst = 0;
    for (uint32_t y = 0; y < bgra.height(); ++y) {
        for (uint32_t x = 0; x < bgra.width(); ++x) {
            const Reference ref = reference(cs, rgb(x, y, 2), rgb(x, y, 1), rgb(x, y, 0));
            worst = std::max(worst, std::abs(pixel(yuv, 0, x, y) - ref.y));
        }
    }
    for (uint32_t y = 0; y < (bgra.height() + 1) / 2; ++y) {
        for (uint32_t x = 0; x < (bgra.width() + 1) / 2; ++x) {
            double mean[3] = {};
            for (int c = 0; c < 3; ++c) {
                mean[c] = (rgb(2 * x, 2 * y, c) + rgb(2 * x + 1, 2 * y, c) +
                           rgb(2 * x, 2 * y + 1, c) + rgb(2 * x + 1, 2 * y + 1, c)) /
                          4;
            }
            const Reference ref = reference(cs, mean[2], mean[1], mean[0]);
            const bool nv12 = yuv.format() == PixelFormat::NV12;
            const uint8_t cb = nv12 ? pixel(yuv, 1, 2 * x, y) : pixel(yuv, 1, x, y);
            const uint8_t cr = nv12 ? pixel(yuv, 1, 2 * x + 1, y) : pixel(yuv, 2, x, y);
            worst = std::max({worst, std::abs(cb - ref.cb), std::abs(cr - ref.cr)});
        }
    }
    return worst;
}

}  // namespace

TEST(VideoConvert, BgraToYuvMatchesTheReference) {
    for (PixelFormat format : {PixelFormat::I420, PixelFormat::NV12}) {
        for (const ColorSpace& cs : kColorSpaces) {
            // Odd sizes reach the lone last column and row.
            for (auto [w, h] : {std::pair{64u, 48u}, std::pair{333u, 199u}, std::pair{1u, 1u}}) {
                const VideoFrame bgra = random_bgra(w, h, w * h);
                Result<VideoFrame> yuv = convert_frame(bgra, format, cs);
                ASSERT_TRUE(yuv);
                EXPECT_EQ(yuv->format(), format);
                EXPECT_EQ(yuv->width(), w);
                EXPECT_EQ(yuv->height(), h);
                EXPECT_EQ(yuv->color_space(), cs);
                EXPECT_EQ(yuv->capture_time(), bgra.capture_time());
                EXPECT_LE(max_error(bgra, *yuv), 1.0) << int(format) << " " << int(cs.matrix)
                                                      << int(cs.range) << " " << w << "x" << h;
            }
        }
    }
}

TEST(VideoConvert, BlackWhiteAndGreyAreExact) {
    struct Case {
        ColorRange range;
        uint8_t rgb, y;
    };
    for (const Case& c : {Case{ColorRange::Limited, 0, 16}, Case{ColorRange::Limited, 255, 235},
                          Case{ColorRange::Full, 0, 0}, Case{ColorRange::Full, 255, 255},
                          Case{ColorRange::Full, 128, 128}}) {
        for (ColorMatrix matrix : {ColorMatrix::BT709, ColorMatrix::BT601}) {
            Result<VideoFrame> yuv = convert_frame(solid_bgra(4, 4, c.rgb, c.rgb, c.rgb),
                                                   PixelFormat::I420, {matrix, c.range});
            ASSERT_TRUE(yuv);
            EXPECT_EQ(pixel(*yuv, 0, 3, 3), c.y) << int(c.rgb);
            EXPECT_EQ(pixel(*yuv, 1, 1, 1), 128);
            EXPECT_EQ(pixel(*yuv, 2, 1, 1), 128);
        }
    }
}

TEST(VideoConvert, SaturatedColoursStayInRange) {
    // Full-range blue has Cb of 128 + 127.5: it must clamp, not wrap to 0.
    Result<VideoFrame> blue = convert_frame(solid_bgra(2, 2, 0, 0, 255), PixelFormat::NV12,
                                            {ColorMatrix::BT709, ColorRange::Full});
    ASSERT_TRUE(blue);
    EXPECT_EQ(pixel(*blue, 1, 0, 0), 255);

    Result<VideoFrame> red = convert_frame(solid_bgra(2, 2, 255, 0, 0), PixelFormat::NV12,
                                           {ColorMatrix::BT709, ColorRange::Limited});
    ASSERT_TRUE(red);
    EXPECT_EQ(pixel(*red, 1, 1, 0), 240);  // Cr
}

TEST(VideoConvert, TheStampSurvivesConversion) {
    const Timestamp time{42'000'017us};
    const VideoFrame bgra = draw_pattern({PixelFormat::BGRA, 320, 180}, 3, time);
    for (PixelFormat format : {PixelFormat::I420, PixelFormat::NV12}) {
        Result<VideoFrame> yuv = convert_frame(bgra, format);
        ASSERT_TRUE(yuv);
        EXPECT_EQ(read_timestamp(*yuv), time);
    }
}

TEST(VideoConvert, TheSameFormatIsCopied) {
    VideoFrame picture = draw_pattern({PixelFormat::NV12, 100, 66}, 0, Timestamp{3s});
    picture.set_color_space({ColorMatrix::BT601, ColorRange::Full});
    Result<VideoFrame> copy = convert_frame(picture, PixelFormat::NV12);
    ASSERT_TRUE(copy);
    EXPECT_NE(copy->plane(0).data, picture.plane(0).data);
    EXPECT_EQ(copy->color_space(), picture.color_space());  // a copy keeps its colour space
    EXPECT_EQ(copy->capture_time(), picture.capture_time());
    for (size_t i = 0; i < 2; ++i) {
        for (uint32_t y = 0; y < plane_rows(PixelFormat::NV12, i, 66); ++y) {
            EXPECT_EQ(std::memcmp(copy->plane(i).data + y * copy->plane(i).stride,
                                  picture.plane(i).data + y * picture.plane(i).stride,
                                  plane_row_bytes(PixelFormat::NV12, i, 100)),
                      0);
        }
    }
}

TEST(VideoConvert, OneYuvLayoutIntoTheOther) {
    VideoFrame nv12 = draw_pattern({PixelFormat::NV12, 102, 66}, 4, Timestamp{9s});
    nv12.set_color_space({ColorMatrix::BT601, ColorRange::Full});
    Result<VideoFrame> i420 = convert_frame(nv12, PixelFormat::I420);
    ASSERT_TRUE(i420);
    EXPECT_EQ(i420->format(), PixelFormat::I420);
    EXPECT_EQ(i420->color_space(), nv12.color_space());
    EXPECT_EQ(i420->capture_time(), nv12.capture_time());
    for (uint32_t y = 0; y < 33; ++y) {
        for (uint32_t x = 0; x < 51; ++x) {
            ASSERT_EQ(pixel(*i420, 1, x, y), pixel(nv12, 1, 2 * x, y));
            ASSERT_EQ(pixel(*i420, 2, x, y), pixel(nv12, 1, 2 * x + 1, y));
        }
    }

    Result<VideoFrame> back = convert_frame(*i420, PixelFormat::NV12);
    ASSERT_TRUE(back);
    for (size_t i = 0; i < 2; ++i) {
        for (uint32_t y = 0; y < plane_rows(PixelFormat::NV12, i, 66); ++y) {
            EXPECT_EQ(std::memcmp(back->plane(i).data + y * back->plane(i).stride,
                                  nv12.plane(i).data + y * nv12.plane(i).stride,
                                  plane_row_bytes(PixelFormat::NV12, i, 102)),
                      0);
        }
    }
}

TEST(VideoConvert, RefusesWhatItCannotDo) {
    const VideoFrame i420 = draw_pattern({PixelFormat::I420, 64, 64}, 0, Timestamp{});
    EXPECT_EQ(convert_frame(i420, PixelFormat::BGRA).error(), Errc::Unsupported);
    EXPECT_EQ(convert_frame(VideoFrame{}, PixelFormat::NV12).error(), Errc::InvalidArgument);
}

TEST(VideoConvert, RgbFromYuvUndoesTheConversion) {
    // Colours in 2 x 2 blocks, so that chroma is each block's own.
    constexpr uint8_t kColours[][3] = {{0, 0, 0},   {255, 255, 255}, {255, 0, 0},    {0, 255, 0},
                                       {0, 0, 255}, {128, 128, 128}, {200, 120, 40}, {17, 90, 230}};
    MutableVideoFrame frame = MutableVideoFrame::allocate(PixelFormat::BGRA, 16, 2);
    for (uint32_t y = 0; y < 2; ++y) {
        uint8_t* row = frame.plane(0).data + y * frame.plane(0).stride;
        for (uint32_t x = 0; x < 16; ++x) {
            const uint8_t* rgb = kColours[x / 2];
            row[4 * x] = rgb[2];
            row[4 * x + 1] = rgb[1];
            row[4 * x + 2] = rgb[0];
            row[4 * x + 3] = 0xFF;
        }
    }
    const VideoFrame bgra = std::move(frame).freeze(Timestamp{});
    for (const ColorSpace& cs : kColorSpaces) {
        Result<VideoFrame> yuv = convert_frame(bgra, PixelFormat::I420, cs);
        ASSERT_TRUE(yuv);
        const YuvToRgb matrix = rgb_from_yuv(cs);
        for (uint32_t x = 0; x < 16; x += 2) {
            const float sample[3] = {pixel(*yuv, 0, x, 0) / 255.0f,
                                     pixel(*yuv, 1, x / 2, 0) / 255.0f,
                                     pixel(*yuv, 2, x / 2, 0) / 255.0f};
            for (int c = 0; c < 3; ++c) {
                const auto& m = matrix[c];
                const float value = m[0] * sample[0] + m[1] * sample[1] + m[2] * sample[2] + m[3];
                // Within the levels the 8-bit samples round away.
                EXPECT_NEAR(value * 255, kColours[x / 2][c], 1.5)
                    << "colour " << x / 2 << " channel " << c << " " << int(cs.matrix)
                    << int(cs.range);
            }
        }
    }
}
