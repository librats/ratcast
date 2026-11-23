#include "ratcast/client/size_adapter.h"

#include <gtest/gtest.h>

#include <chrono>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr Timestamp kStart{100s};

/// A screen of 4K, fed to an adapter in steps of 100 ms (a picture each),
/// at the bitrate the test sets and with as many skipped as it says.
class Screen {
public:
    explicit Screen(SizeAdapterConfig config = {}, uint32_t width = 3840, uint32_t height = 2160)
        : adapter_(config, kStart), width_(width), height_(height) {}

    /// For `time`, at `bitrate`, a share of `skipped_share` of the pictures
    /// skipped: the limit then.
    uint32_t run(Duration time, uint32_t bitrate, double skipped_share = 0) {
        for (Duration done{}; done < time; done += 100ms) {
            now_ += 100ms;
            // Six pictures a step: 60 a second.
            pictures_ += 6;
            skipped_ += uint64_t(6 * skipped_share + 0.5);
            const uint32_t lines = adapter_.lines();
            // What the publisher hands on: no larger than the limit.
            const bool limited = lines != 0 && std::min(width_, height_) > lines;
            const uint32_t width = limited ? uint32_t(uint64_t(width_) * lines / height_) : width_;
            const uint32_t height = limited ? lines : height_;
            adapter_.update(now_, width, height, bitrate, pictures_, skipped_);
        }
        return adapter_.lines();
    }

    /// How long until the limit is other than it is, at most `within`.
    Duration until_change(uint32_t bitrate, Duration within, double skipped_share = 0) {
        const uint32_t before = adapter_.lines();
        for (Duration waited{}; waited < within; waited += 100ms) {
            if (run(100ms, bitrate, skipped_share) != before) return waited + 100ms;
        }
        return within;
    }

    uint32_t lines() const { return adapter_.lines(); }

private:
    SizeAdapter adapter_;
    const uint32_t width_;
    const uint32_t height_;
    Timestamp now_ = kStart;
    uint64_t pictures_ = 0;
    uint64_t skipped_ = 0;
};

}  // namespace

TEST(BitrateForSize, ScalesWithPixelsLessThanInProportion) {
    EXPECT_EQ(bitrate_for_size(1920, 1080, 60, 8'000'000), 8'000'000u);
    EXPECT_NEAR(bitrate_for_size(3840, 2160, 60, 8'000'000), 22'627'000, 1000);
    EXPECT_NEAR(bitrate_for_size(2560, 1440, 60, 8'000'000), 12'317'000, 1000);
    EXPECT_NEAR(bitrate_for_size(1280, 720, 60, 8'000'000), 4'354'000, 1000);
    EXPECT_NEAR(bitrate_for_size(1920, 1080, 30, 8'000'000), 4'757'000, 1000) << "half the frames";
    EXPECT_EQ(bitrate_for_size(320, 240, 30, 8'000'000), 1'000'000u) << "never less";
}

TEST(SizeAdapter, KeepsTheScreenWhereThePathCarriesIt) {
    Screen screen;
    EXPECT_EQ(screen.lines(), 0u);
    EXPECT_EQ(screen.run(60s, 20'000'000), 0u);
}

TEST(SizeAdapter, StepsDownToWhatANarrowPathKeeps) {
    Screen screen;
    // 4 Mbit/s keeps 1080 lines (2.8 needed) but not 1440 (4.3): straight
    // there, five seconds' settling and three of too little on.
    EXPECT_EQ(screen.run(7s, 4'000'000), 0u) << "rate control finds its feet first";
    EXPECT_EQ(screen.run(2s, 4'000'000), 1080u);
    // And no further, nor up: 1440 would need 7.4.
    EXPECT_EQ(screen.run(120s, 4'000'000), 1080u);
}

TEST(SizeAdapter, GoesNoLowerThanTheLeast) {
    Screen screen;
    EXPECT_EQ(screen.run(60s, 500'000), 720u);
}

TEST(SizeAdapter, AMomentsDipIsNoReason) {
    Screen screen;
    screen.run(10s, 20'000'000);
    screen.run(2s, 3'000'000);
    EXPECT_EQ(screen.run(30s, 20'000'000), 0u);
}

TEST(SizeAdapter, StepsBackUpAsThePathGrows) {
    Screen screen;
    EXPECT_EQ(screen.run(20s, 3'000'000), 1080u);
    // 8 Mbit/s carries 1440 lines (7.4 to go up) but not yet 4K (13.6).
    const Duration up = screen.until_change(8'000'000, 60s);
    EXPECT_GE(up, 10s);
    EXPECT_LT(up, 11s);
    EXPECT_EQ(screen.lines(), 1440u);
    EXPECT_EQ(screen.run(120s, 8'000'000), 1440u);
    // And as it comes, past the last level, at 20.
    EXPECT_EQ(screen.run(40s, 20'000'000), 0u);
}

TEST(SizeAdapter, AStepUpThatComesBackWaitsLongerNextTime) {
    Screen screen;
    screen.run(20s, 3'000'000);                               // 1080
    ASSERT_EQ(screen.until_change(8'000'000, 60s) / 1s, 10);  // 1440
    // Too much after all: back down within seconds вЂ” eight, a second ago.
    screen.run(9s, 3'000'000);
    ASSERT_EQ(screen.lines(), 1080u);
    // Now, the four seconds left of its settling over, twice as long.
    const Duration up = screen.until_change(8'000'000, 60s);
    EXPECT_GE(up, 24s);
    EXPECT_LT(up, 25s);
}

TEST(SizeAdapter, AnEncoderBehindStepsDownForGood) {
    Screen screen;
    screen.run(6s, 20'000'000);
    // Two pictures in six skipped: a third, seen within two counts.
    const Duration down = screen.until_change(20'000'000, 10s, 1.0 / 3);
    EXPECT_LE(down, 4s);
    EXPECT_EQ(screen.lines(), 1440u);
    // Keeping up there, with a path that carries 4K: never up again.
    EXPECT_EQ(screen.run(300s, 20'000'000), 1440u);
}

TEST(SizeAdapter, AFewSkippedAreNoReason) {
    Screen screen;
    EXPECT_EQ(screen.run(60s, 20'000'000, 1.0 / 6), 0u);
}

TEST(SizeAdapter, StaysWithinItsMost) {
    Screen screen({.max_lines = 1080});
    EXPECT_EQ(screen.lines(), 1080u);
    EXPECT_EQ(screen.run(120s, 20'000'000), 1080u);
    EXPECT_EQ(screen.run(20s, 1'000'000), 720u);
    EXPECT_EQ(screen.run(120s, 20'000'000), 1080u);
}

TEST(SizeAdapter, ASmallWindowHasNothingToGive) {
    Screen screen({}, 1024, 640);
    EXPECT_EQ(screen.run(60s, 500'000), 0u) << "720 is no step down from 640";
}

TEST(SizeAdapter, AScreenOf1200LinesGoesTo1080) {
    Screen screen({}, 1920, 1200);
    EXPECT_EQ(screen.run(20s, 2'900'000), 1080u);
}
