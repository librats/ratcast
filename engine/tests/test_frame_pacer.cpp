#include "capture/frame_pacer.h"

#include <gtest/gtest.h>

#include <random>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

/// How many of `count` pictures, `period` apart with up to `jitter` either
/// way, a pacer at `rate` lets through.
int admitted(uint32_t rate, Duration period, int count, Duration jitter = {}) {
    FramePacer pacer(rate);
    std::mt19937 rng(1);
    std::uniform_int_distribution<int64_t> noise(-jitter.count(), jitter.count());
    int passed = 0;
    for (int i = 0; i < count; ++i) {
        const Timestamp at{10s + period * i + Duration{noise(rng)}};
        passed += pacer.admit(at) ? 1 : 0;
    }
    return passed;
}

}  // namespace

TEST(FramePacer, NoLimitLetsEverythingThrough) { EXPECT_EQ(admitted(0, 1ms, 1000), 1000); }

TEST(FramePacer, AFasterSourceIsThinnedToTheRate) {
    EXPECT_NEAR(admitted(60, 6944us, 144), 60, 1);    // a second of 144 Hz
    EXPECT_NEAR(admitted(30, 16667us, 600), 300, 1);  // ten seconds of 60 Hz
}

TEST(FramePacer, ASourceAtTheRateKeepsEveryPictureDespiteJitter) {
    EXPECT_EQ(admitted(60, 16667us, 600, 3ms), 600);
}

TEST(FramePacer, ASlowerSourceKeepsEveryPicture) {
    EXPECT_EQ(admitted(60, 33333us, 300, 2ms), 300);
}

TEST(FramePacer, ResumesAtOnceAfterAPause) {
    FramePacer pacer(60);
    EXPECT_TRUE(pacer.admit(Timestamp{1s}));
    EXPECT_FALSE(pacer.admit(Timestamp{1s + 5ms}));
    EXPECT_TRUE(pacer.admit(Timestamp{5s}));  // the screen was still for a while
    EXPECT_FALSE(pacer.admit(Timestamp{5s + 5ms}));
    EXPECT_TRUE(pacer.admit(Timestamp{5s + 17ms}));
}
