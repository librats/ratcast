#include "ratcast/stream/fastest_transit.h"

#include <gtest/gtest.h>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

const Timestamp kStart{10s};

}  // namespace

TEST(FastestTransitTest, IsTheLeastOfLate) {
    FastestTransit fastest(4s);
    EXPECT_FALSE(fastest.least());
    fastest.add(50ms, kStart);
    fastest.add(30ms, kStart + 10ms);
    fastest.add(70ms, kStart + 20ms);
    EXPECT_EQ(fastest.least(), 30ms);
}

TEST(FastestTransitTest, FollowsAPathThatSlowsWithinMemoryAndOneThatSpeedsUpAtOnce) {
    FastestTransit fastest(4s);  // four periods of a second
    fastest.add(30ms, kStart);
    for (int i = 1; i < 40; ++i) fastest.add(80ms, kStart + 100ms * i);
    EXPECT_EQ(fastest.least(), 30ms) << "remembered for the period it came in and three more";
    fastest.add(80ms, kStart + 4s);  // a fifth period: the first goes
    EXPECT_EQ(fastest.least(), 80ms);
    fastest.add(20ms, kStart + 4100ms);
    EXPECT_EQ(fastest.least(), 20ms);
}
