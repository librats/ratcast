#include "ratcast/core/time.h"

#include <gtest/gtest.h>

using namespace ratcast;
using namespace std::chrono_literals;

TEST(SteadyClock, NeverGoesBack) {
    const Clock& clock = SteadyClock::instance();
    const Timestamp first = clock.now();
    const Timestamp second = clock.now();
    EXPECT_LE(first, second);
}

TEST(ManualClock, MovesOnlyWhenTold) {
    ManualClock clock(Timestamp{5s});
    EXPECT_EQ(clock.now(), Timestamp{5s});
    EXPECT_EQ(clock.now(), Timestamp{5s});

    clock.advance(1500us);
    EXPECT_EQ(clock.now(), Timestamp{5s + 1500us});

    clock.set(Timestamp{1s});
    EXPECT_EQ(clock.now(), Timestamp{1s});
}

TEST(ManualClock, StandsInForTheRealClock) {
    ManualClock manual;
    const Clock& clock = manual;
    manual.advance(20ms);
    EXPECT_EQ(clock.now().time_since_epoch(), 20ms);
}
