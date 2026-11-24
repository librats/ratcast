#include "ratcast/client/size_adapter.h"

#include <gtest/gtest.h>

#include <chrono>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr Timestamp kStart{100s};

/// A screen of 4K, fed to an adapter in steps of 100 ms, at the bitrate the
/// test sets, its pictures coming at the rate set (60 a second at first),
/// each taking the encoder the time set (5 ms at first), and as many skipped
/// as the test says.
class Screen {
public:
    explicit Screen(SizeAdapterConfig config = {}, uint32_t width = 3840, uint32_t height = 2160)
        : adapter_(config, kStart), width_(width), height_(height) {}

    /// Pictures a second from now on.
    void set_rate(double rate) { rate_ = rate; }
    /// What each picture takes the encoder from now on.
    void set_cost(Duration cost) { cost_ = cost; }

    /// For `time`, at `bitrate`, a share of `skipped_share` of the pictures
    /// skipped: the limit then.
    uint32_t run(Duration time, uint32_t bitrate, double skipped_share = 0) {
        for (Duration done{}; done < time; done += 100ms) {
            now_ += 100ms;
            due_ += rate_ / 10;
            const auto given = uint64_t(due_);
            due_ -= double(given);
            const auto skipped = uint64_t(double(given) * skipped_share + 0.5);
            counts_.pictures += given;
            counts_.skipped += skipped;
            counts_.taken += given - skipped;
            counts_.busy += cost_ * int64_t(given - skipped);
            const uint32_t lines = adapter_.lines();
            // What the publisher hands on: no larger than the limit.
            const bool limited = lines != 0 && std::min(width_, height_) > lines;
            const uint32_t width = limited ? uint32_t(uint64_t(width_) * lines / height_) : width_;
            const uint32_t height = limited ? lines : height_;
            adapter_.update(now_, width, height, bitrate, counts_);
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

    /// The encoder opening anew for `time`, as when the window shared changes
    /// size: the publisher asks nothing meanwhile, and of the pictures that
    /// come, each but the last is replaced by the next — skipped.
    void reopen(Duration time) {
        const auto count = uint64_t(rate_ * std::chrono::duration<double>(time).count());
        now_ += time;
        counts_.pictures += count;
        counts_.skipped += count - 1;
        adapter_.encoder_started(now_);
    }

    uint32_t lines() const { return adapter_.lines(); }

private:
    SizeAdapter adapter_;
    const uint32_t width_;
    const uint32_t height_;
    Timestamp now_ = kStart;
    double rate_ = 60;
    double due_ = 0;  ///< pictures owed, less than one
    Duration cost_ = 5ms;
    EncoderCounts counts_;
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

// A page scrolled at 60 a second, each picture of 4K taking the encoder
// 18 ms: too much, and so for as long as the pictures come as fast — but for
// a try once what was measured is five minutes old, and after that try fell
// behind again, ten.
TEST(SizeAdapter, AnEncoderBehindStepsDownAndTriesAgainEverLessOften) {
    Screen screen;
    screen.set_cost(18ms);
    screen.run(6s, 20'000'000);
    // Two pictures in six skipped: a third, seen within two counts.
    const Duration down = screen.until_change(20'000'000, 10s, 1.0 / 3);
    EXPECT_LE(down, 4s);
    EXPECT_EQ(screen.lines(), 1440u);
    // Keeping up there, with a path that carries 4K: not up again for now.
    EXPECT_EQ(screen.run(280s, 20'000'000), 1440u);

    // Five minutes on, ten seconds held: tried again.
    const Duration tried = screen.until_change(20'000'000, 60s);
    EXPECT_GE(tried, 25s);
    EXPECT_LE(tried, 35s);
    ASSERT_EQ(screen.lines(), 2160u);
    ASSERT_LE(screen.until_change(20'000'000, 10s, 1.0 / 3), 8s) << "settled, then a count";
    ASSERT_EQ(screen.lines(), 1440u);

    // Ten minutes now, and twenty seconds held, the try having come back.
    EXPECT_EQ(screen.run(590s, 20'000'000), 1440u);
    const Duration again = screen.until_change(20'000'000, 60s);
    EXPECT_GE(again, 25s);
    EXPECT_LE(again, 35s);
    EXPECT_EQ(screen.lines(), 2160u);
}

// The same, then a film at 24 a second: 18 ms in 42 is well in reach, and
// the path carries 4K. Seen in a session's log: 28 minutes of a film at 1440
// lines, the encoder having fallen behind at 4K in a browser's first seconds.
TEST(SizeAdapter, AFilmGoesBackUpWhereAPageScrolledWasTooMuch) {
    Screen screen;
    screen.set_cost(18ms);
    screen.run(6s, 20'000'000);
    ASSERT_LE(screen.until_change(20'000'000, 10s, 1.0 / 3), 4s);
    ASSERT_EQ(screen.lines(), 1440u);

    screen.set_rate(24);
    screen.set_cost(8ms);
    // Five seconds' settling, a count of two to see the rate, ten held.
    const Duration up = screen.until_change(20'000'000, 60s);
    EXPECT_GE(up, 16s);
    EXPECT_LE(up, 19s);
    EXPECT_EQ(screen.lines(), 2160u);
    EXPECT_EQ(screen.run(300s, 20'000'000), 0u) << "and as it comes, the same pictures";
}

// The same with a video at 30 a second, as many are: 18 ms in 33 keeps up.
// At 45, 18 ms in 22 is too close.
TEST(SizeAdapter, AVideoAt30GoesBackUpToo) {
    Screen screen;
    screen.set_cost(18ms);
    screen.run(6s, 20'000'000);
    ASSERT_LE(screen.until_change(20'000'000, 10s, 1.0 / 3), 4s);
    ASSERT_EQ(screen.lines(), 1440u);

    screen.set_rate(45);
    EXPECT_EQ(screen.run(60s, 20'000'000), 1440u);
    screen.set_rate(30);
    EXPECT_LE(screen.until_change(20'000'000, 15s), 13s) << "a count to see the rate, ten held";
    EXPECT_EQ(screen.lines(), 2160u);
}

// Back up for a film, then the pictures come at 60 again: behind as before,
// down at once, and the next try waits twice as long.
TEST(SizeAdapter, FallingBehindAgainSoonAfterWaitsLongerNextTime) {
    Screen screen;
    screen.set_cost(18ms);
    screen.run(6s, 20'000'000);
    ASSERT_LE(screen.until_change(20'000'000, 10s, 1.0 / 3), 4s);
    screen.set_rate(24);
    const Duration first = screen.until_change(20'000'000, 60s);
    ASSERT_EQ(screen.lines(), 2160u);

    screen.set_rate(60);
    ASSERT_LE(screen.until_change(20'000'000, 10s, 1.0 / 3), 8s) << "settled, then a count";
    ASSERT_EQ(screen.lines(), 1440u);
    screen.set_rate(24);
    const Duration second = screen.until_change(20'000'000, 60s);
    EXPECT_EQ(screen.lines(), 2160u);
    EXPECT_GE(second - first, 9s);
    EXPECT_LE(second - first, 11s);
}

// A still screen says nothing of what the encoder keeps up with: no try up
// for a page being read between scrolls, before what was measured is old.
TEST(SizeAdapter, AStillScreenIsNoReasonToTryAgain) {
    Screen screen;
    screen.set_cost(18ms);
    screen.run(6s, 20'000'000);
    ASSERT_LE(screen.until_change(20'000'000, 10s, 1.0 / 3), 4s);
    screen.set_rate(2);
    EXPECT_EQ(screen.run(280s, 20'000'000), 1440u);
}

// Nor once what was measured is old: the try waits for a scroll, and having
// fallen behind straight away, the next waits twice as long. Tried on the
// still page, it would have held a minute and been counted as having kept
// up: a try every five minutes, each coming back at the next scroll.
TEST(SizeAdapter, AStillScreenWaitsForMovementToTryAgain) {
    Screen screen;
    screen.set_cost(18ms);
    screen.run(6s, 20'000'000);
    ASSERT_LE(screen.until_change(20'000'000, 10s, 1.0 / 3), 4s);
    ASSERT_EQ(screen.lines(), 1440u);
    screen.set_rate(2);
    EXPECT_EQ(screen.run(600s, 20'000'000), 1440u);

    // Scrolled: a count to see the rate, ten seconds held.
    screen.set_rate(60);
    EXPECT_LE(screen.until_change(20'000'000, 30s), 13s);
    ASSERT_EQ(screen.lines(), 2160u);
    ASSERT_LE(screen.until_change(20'000'000, 10s, 1.0 / 3), 8s) << "settled, then a count";
    ASSERT_EQ(screen.lines(), 1440u);

    // Ten minutes now, and twenty seconds held, the try having come back.
    EXPECT_EQ(screen.run(590s, 20'000'000), 1440u);
    const Duration again = screen.until_change(20'000'000, 60s);
    EXPECT_GE(again, 25s);
    EXPECT_LE(again, 35s);
    EXPECT_EQ(screen.lines(), 2160u);
}

// Back up for a film at 24, which then takes the encoder 50 ms a picture at
// 4K (the GPU busy with a game, say): behind at 24 as at 60 — down again,
// and not back up for it until what was measured is five minutes old. The
// game over by then, 4K is kept: what one moment measured is no ceiling.
TEST(SizeAdapter, AFilmTooMuchForTheEncoderStepsDown) {
    Screen screen;
    screen.set_cost(18ms);
    screen.run(6s, 20'000'000);
    ASSERT_LE(screen.until_change(20'000'000, 10s, 1.0 / 3), 4s);
    screen.set_rate(24);
    ASSERT_LE(screen.until_change(20'000'000, 60s), 19s);
    ASSERT_EQ(screen.lines(), 2160u);

    screen.set_cost(50ms);
    EXPECT_LE(screen.until_change(20'000'000, 10s, 1.0 / 3), 8s) << "settled, then a count";
    EXPECT_EQ(screen.lines(), 1440u);
    screen.set_cost(20ms);
    EXPECT_EQ(screen.run(280s, 20'000'000), 1440u);
    // Five minutes on, twenty seconds held (the step up had come straight back).
    EXPECT_LE(screen.until_change(20'000'000, 60s), 45s);
    EXPECT_EQ(screen.lines(), 2160u);
    EXPECT_EQ(screen.run(300s, 20'000'000), 0u) << "kept, and as it comes";
}

// A screen still but for a picture now and then, one of two replaced before
// the encoder took it: too few to say anything of the encoder.
TEST(SizeAdapter, APictureOrTwoOfAStillScreenSkippedIsNoReason) {
    Screen screen;
    screen.set_rate(2);
    EXPECT_EQ(screen.run(60s, 20'000'000, 0.5), 0u);
}

// An encoder stuck on one picture for a whole count (every other replaced
// while it works): no picture done, no time counted — yet it kept up with
// nothing, and the share stays down while pictures come as fast.
TEST(SizeAdapter, AnEncoderStuckOnAPictureKeepsUpWithNothing) {
    Screen screen;
    ASSERT_LE(screen.until_change(20'000'000, 10s, 1.0), 8s) << "settled, then a count";
    ASSERT_EQ(screen.lines(), 1440u);
    EXPECT_EQ(screen.run(280s, 20'000'000), 1440u);
}

// A window shared at 1440 lines going full screen and back: each change of
// its size opens the encoder anew, half a second in which every picture is
// skipped, and a few more as it finds its feet. No reason to step down.
TEST(SizeAdapter, AnEncoderStartingOverIsNoReason) {
    Screen screen({}, 2304, 1440);
    screen.run(6s, 20'000'000);
    for (int i = 0; i < 4; ++i) {
        screen.reopen(500ms);
        screen.run(1s, 20'000'000, 1.0 / 3);
        screen.reopen(500ms);
        screen.run(4s, 20'000'000);
    }
    EXPECT_EQ(screen.lines(), 0u);
}

// What it skips once it has found its feet counts as ever.
TEST(SizeAdapter, AnEncoderBehindAfterStartingOverStepsDown) {
    Screen screen;
    screen.run(6s, 20'000'000);
    screen.reopen(500ms);
    const Duration down = screen.until_change(20'000'000, 15s, 1.0 / 3);
    // Five seconds not counted, then within two counts.
    EXPECT_GE(down, 5s);
    EXPECT_LE(down, 9s);
    EXPECT_EQ(screen.lines(), 1440u);
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
