#include "ratcast/engine/pattern_source.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr PixelFormat kFormats[] = {PixelFormat::I420, PixelFormat::NV12, PixelFormat::BGRA};

/// A writable copy of `picture`, for damaging it.
MutableVideoFrame copy_of(const VideoFrame& picture) {
    MutableVideoFrame copy =
        MutableVideoFrame::allocate(picture.format(), picture.width(), picture.height());
    for (size_t i = 0; i < picture.plane_count(); ++i) {
        const size_t row = plane_row_bytes(picture.format(), i, picture.width());
        for (size_t y = 0; y < plane_rows(picture.format(), i, picture.height()); ++y) {
            std::memcpy(copy.plane(i).data + y * copy.plane(i).stride,
                        picture.plane(i).data + y * picture.plane(i).stride, row);
        }
    }
    return copy;
}

}  // namespace

TEST(Pattern, StampedTimeReadsBackInEveryFormat) {
    for (PixelFormat format : kFormats) {
        for (auto [w, h] : {std::pair{64u, 64u}, std::pair{1920u, 1080u}, std::pair{333u, 199u}}) {
            const Timestamp time{123'456'789'012us};
            const VideoFrame picture = draw_pattern({format, w, h}, 7, time);
            EXPECT_EQ(read_timestamp(picture), time) << int(format) << " " << w << "x" << h;
            EXPECT_EQ(picture.capture_time(), time);
        }
    }
}

TEST(Pattern, StampSurvivesNoise) {
    const Timestamp time{987'654'321us};
    MutableVideoFrame noisy = copy_of(draw_pattern({PixelFormat::I420, 320, 240}, 0, time));
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> noise(-50, 50);
    for (size_t y = 0; y < 240; ++y) {
        for (size_t x = 0; x < 320; ++x) {
            uint8_t& p = noisy.plane(0).data[y * noisy.plane(0).stride + x];
            p = static_cast<uint8_t>(std::clamp(p + noise(rng), 0, 255));
        }
    }
    EXPECT_EQ(read_timestamp(std::move(noisy).freeze(Timestamp{})), time);
}

TEST(Pattern, ADamagedStampIsRejected) {
    MutableVideoFrame damaged =
        copy_of(draw_pattern({PixelFormat::I420, 256, 256}, 0, Timestamp{5s}));
    // Invert block 3 of the top row: one bit of the stamp.
    const uint32_t block = 256 / 4 / 8;
    for (uint32_t y = 0; y < block; ++y) {
        for (uint32_t x = 3 * block; x < 4 * block; ++x) {
            uint8_t& p = damaged.plane(0).data[y * damaged.plane(0).stride + x];
            p = static_cast<uint8_t>(16 + 235 - p);
        }
    }
    EXPECT_EQ(read_timestamp(std::move(damaged).freeze(Timestamp{})), std::nullopt);
    EXPECT_EQ(read_timestamp(VideoFrame{}), std::nullopt);
}

TEST(Pattern, PicturesMove) {
    const PatternConfig config{PixelFormat::I420, 128, 128};
    const VideoFrame a = draw_pattern(config, 0, Timestamp{});
    const VideoFrame b = draw_pattern(config, 1, Timestamp{});
    // Below the stamp, which takes the top quarter.
    const size_t y = 100;
    EXPECT_NE(std::memcmp(a.plane(0).data + y * a.plane(0).stride,
                          b.plane(0).data + y * b.plane(0).stride, 128),
              0);
}

TEST(PatternSource, DeliversStampedPicturesAtItsRate) {
    PatternSource source({PixelFormat::NV12, 160, 120, 100, 1});
    std::mutex mutex;
    std::vector<Timestamp> stamps;
    ASSERT_TRUE(source.start([&](VideoFrame picture) {
        std::lock_guard lock(mutex);
        stamps.push_back(*read_timestamp(picture));
    }));
    EXPECT_FALSE(source.start([](VideoFrame) {}));  // already running

    std::this_thread::sleep_for(300ms);
    source.stop();
    const size_t delivered = stamps.size();
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(stamps.size(), delivered);  // nothing after stop()

    EXPECT_GE(delivered, 15u);  // ~30 expected; generous for a loaded machine
    EXPECT_LE(delivered, 35u);
    EXPECT_TRUE(std::ranges::is_sorted(stamps));
}
