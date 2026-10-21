// PlayoutClock on a clock the test moves: pictures held to the sound, the
// floor the sound is asked to keep, sides that went quiet, and how far apart
// the two were as they played.

#include "ratcast/client/playout_clock.h"

#include <gtest/gtest.h>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

const Timestamp kCapture{100s};  // the sender's clock

class PlayoutClockTest : public ::testing::Test {
protected:
    ManualClock clock_{Timestamp{500s}};
    PlayoutClock sync_{clock_};
};

TEST_F(PlayoutClockTest, WithoutSoundPicturesKeepTheirOwnTime) {
    const Timestamp own = kCapture + 30ms;
    EXPECT_EQ(sync_.due(kCapture, own, 200ms), own);
    sync_.pictures_need(30ms);
    EXPECT_EQ(sync_.sound_floor(), 0ms);
    sync_.picture_shown(kCapture);
    EXPECT_EQ(sync_.stats().apart.count(), 0u);
}

TEST_F(PlayoutClockTest, PicturesAreHeldUntilTheSoundCapturedWithThemIsHeard) {
    // Heard 60 ms after capture, 25 ms of it the jitter buffer's.
    sync_.sound_played(kCapture, kCapture + 60ms, 25ms);
    EXPECT_EQ(sync_.due(kCapture + 1s, kCapture + 1s + 10ms, 200ms), kCapture + 1s + 60ms);
    // Never held more than asked...
    EXPECT_EQ(sync_.due(kCapture, kCapture + 10ms, 20ms), kCapture + 30ms);
    // ...nor shown sooner than their own schedule says.
    EXPECT_EQ(sync_.due(kCapture, kCapture + 90ms, 200ms), kCapture + 90ms);
}

TEST_F(PlayoutClockTest, TheSoundWaitsForPicturesThatNeedLonger) {
    sync_.sound_played(kCapture, kCapture + 60ms, 25ms);  // 35 ms not the buffer's
    sync_.pictures_need(100ms);
    EXPECT_EQ(sync_.sound_floor(), 65ms);  // so that it is heard 100 ms after capture
    sync_.pictures_need(20ms);
    EXPECT_EQ(sync_.sound_floor(), 0ms);
    EXPECT_EQ(sync_.stats().floor, 0ms);
}

TEST_F(PlayoutClockTest, ASideQuietForASecondNoLongerCounts) {
    sync_.sound_played(kCapture, kCapture + 60ms, 25ms);
    sync_.pictures_need(100ms);
    clock_.advance(1100ms);
    EXPECT_EQ(sync_.sound_floor(), 0ms);
    EXPECT_EQ(sync_.due(kCapture, kCapture + 10ms, 200ms), kCapture + 10ms);
    const PlayoutClockStats stats = sync_.stats();
    EXPECT_FALSE(stats.sound);
    EXPECT_FALSE(stats.pictures);
}

TEST_F(PlayoutClockTest, MeasuresHowFarApartPicturesAndSoundPlay) {
    // The sound is heard 60 ms after capture. A picture shown 80 ms after
    // capture is 20 ms late; one shown 50 ms after, 10 ms early.
    const Timestamp captured = clock_.now() - 80ms;
    sync_.sound_played(captured - 1s, captured - 1s + 60ms, 25ms);
    sync_.picture_shown(captured);
    sync_.picture_shown(captured + 30ms);
    const PlayoutClockStats stats = sync_.stats();
    EXPECT_EQ(stats.apart.count(), 2u);
    EXPECT_EQ(stats.apart.max(), 20ms);
    EXPECT_EQ(stats.mean_apart, 5ms);  // (+20 - 10) / 2
    EXPECT_EQ(stats.sound, 60ms);
}

TEST(PlayoutClockOutOfStepTest, OnlyMeasures) {
    ManualClock clock{Timestamp{500s}};
    PlayoutClock sync(clock, false);
    sync.sound_played(kCapture, kCapture + 60ms, 25ms);
    sync.pictures_need(100ms);
    EXPECT_EQ(sync.due(kCapture, kCapture + 10ms, 200ms), kCapture + 10ms);
    EXPECT_EQ(sync.sound_floor(), 0ms);
    sync.picture_shown(clock.now() - 10ms);  // 50 ms ahead of the sound
    EXPECT_EQ(sync.stats().mean_apart, -50ms);
}

}  // namespace
