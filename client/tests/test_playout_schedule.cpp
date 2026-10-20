#include "ratcast/client/playout_schedule.h"

#include <gtest/gtest.h>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr Duration kFrame = 16'667us;  // 60 fps
constexpr Duration kPath = 30ms;       // the fastest a frame comes

/// A stream at 60 fps whose sender's clock is far from ours, as it is on
/// another host.
class PlayoutScheduleTest : public ::testing::Test {
protected:
    /// The next frame arrives `late` after the fastest; how long after its
    /// arrival it is due.
    Duration frame(Duration late = 0us, bool keyframe = false) {
        const Timestamp capture = Timestamp{1000s} + number_++ * kFrame;
        const Timestamp arrival = Timestamp{50s} + number_ * kFrame + kPath + late;
        schedule.on_frame(capture, arrival, keyframe);
        return schedule.due(capture) - arrival;
    }

    /// Even frames for `how_long`.
    void frames(Duration how_long) {
        for (Duration played{}; played < how_long; played += kFrame) frame();
    }

    /// A late frame, and one on time after it.
    void run(Duration late, bool keyframe = false) {
        frame(late, keyframe);
        frame();
    }

    PlayoutSchedule schedule{200ms};
    int64_t number_ = 0;
};

}  // namespace

TEST_F(PlayoutScheduleTest, EvenFramesAreShownAsTheyCome) {
    frames(1s);
    EXPECT_EQ(frame(), 0us);
    EXPECT_EQ(schedule.delay(), 0us);
}

TEST_F(PlayoutScheduleTest, OneLateRunIsAStallAndTheNextAsLateIsCovered) {
    frames(1s);
    EXPECT_EQ(frame(70ms), -70ms);  // shown late: nothing says the path does this
    EXPECT_EQ(frame(54ms), -54ms);  // nor the frames held up behind it
    frame();
    EXPECT_EQ(schedule.delay(), 0us);

    frames(2s);
    EXPECT_EQ(frame(70ms), 0us);  // again: shown as it comes...
    EXPECT_EQ(schedule.delay(), 70ms);
    EXPECT_EQ(frame(54ms), 16ms);  // ...and the frames behind it played on after it
    frame();
    frames(1s);
    EXPECT_EQ(frame(60ms), 10ms);  // the next is covered
}

TEST_F(PlayoutScheduleTest, CoversTheSecondLatestRun) {
    frames(1s);
    run(30ms);
    run(70ms);
    EXPECT_EQ(schedule.delay(), 30ms);
    run(50ms);
    EXPECT_EQ(schedule.delay(), 50ms);
}

TEST_F(PlayoutScheduleTest, ShrinksSlowlyAsLateRunsAgeOut) {
    frames(1s);
    run(70ms);
    frames(2s);
    run(70ms);
    frames(7s);
    EXPECT_EQ(schedule.delay(), 70ms);
    frames(2s);  // the first run is ten seconds old a second ago
    EXPECT_NEAR(schedule.delay().count(), Duration{70ms - 50ms}.count(), 2'000);  // 5% of a second
    frames(1s);
    EXPECT_EQ(schedule.delay(), 0us);
    EXPECT_EQ(frame(), 0us);
}

TEST_F(PlayoutScheduleTest, RunsAKeyframeIsInMoveNothing) {
    frames(1s);
    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(frame(60ms, true), -60ms);  // took long to send
        EXPECT_EQ(frame(45ms), -45ms);        // and held up the frames behind it
        frame();
    }
    frame(3ms, true);  // on time: the frames it held up, or a new encoder
    frame(40ms);       // was slow with, are its run all the same
    frame();
    EXPECT_EQ(schedule.delay(), 0us);
    frames(200ms);
    run(40ms);
    run(40ms);
    EXPECT_EQ(schedule.delay(), 40ms);
}

TEST_F(PlayoutScheduleTest, RunsJustBeforeAKeyframeAreItsToo) {
    // A new bitrate: the encoder opening for it slows the one at work.
    frames(1s);
    run(30ms);
    run(40ms);
    EXPECT_EQ(schedule.delay(), 30ms);
    frames(100ms);
    frame(5ms, true);  // the new encoder's first frame
    frames(1s);
    EXPECT_LT(schedule.delay(), 30ms);  // shrinking: neither run counts
    frames(1s);
    EXPECT_EQ(schedule.delay(), 0us);
}

TEST_F(PlayoutScheduleTest, RunsLaterThanTheMostMoveNothing) {
    frames(1s);
    run(300ms);
    run(300ms);
    EXPECT_EQ(schedule.delay(), 0us);
    EXPECT_EQ(frame(300ms), -300ms);
    frame();
    run(150ms);
    run(150ms);
    EXPECT_EQ(schedule.delay(), 150ms);
}

TEST_F(PlayoutScheduleTest, APathThatGotSlowerForGoodIsNoRunToCover) {
    frames(1s);
    // Every frame 40 ms slower from now on: one long run, the frames even...
    for (Duration played{}; played < 30s; played += kFrame) {
        ASSERT_LE(frame(40ms), 0us);
    }
    // ...and, once the fastest frame is one of them, on time.
    EXPECT_EQ(schedule.delay(), 0us);
    EXPECT_EQ(frame(40ms), 0us);
}

TEST_F(PlayoutScheduleTest, WithNoDelayAllowedEverythingIsShownAsItComes) {
    schedule.set_max_delay(0us);
    frames(1s);
    run(70ms);
    run(70ms);
    EXPECT_EQ(frame(70ms), -70ms);
    EXPECT_EQ(schedule.delay(), 0us);
}

TEST_F(PlayoutScheduleTest, ALowerMostCutsTheDelayAtOnce) {
    frames(1s);
    run(150ms);
    run(150ms);
    schedule.set_max_delay(100ms);
    EXPECT_EQ(schedule.delay(), 100ms);
}

TEST_F(PlayoutScheduleTest, ResetForgetsThePath) {
    frames(1s);
    run(70ms);
    run(70ms);
    schedule.reset();
    EXPECT_EQ(schedule.delay(), 0us);
    EXPECT_EQ(frame(), 0us);
}
