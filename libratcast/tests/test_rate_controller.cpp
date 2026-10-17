#include "ratcast/session/rate_controller.h"

#include <gtest/gtest.h>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr BitrateLimits kLimits{.min = 250'000, .start = 2'000'000, .max = 8'000'000};
constexpr Duration kLifetime = 200ms;
constexpr Duration kRtt = 50ms;

PathStats sample(uint64_t bandwidth_bps, bool app_limited = true) {
    PathStats s;
    s.valid = true;
    s.bandwidth_bps = bandwidth_bps;
    s.app_limited = app_limited;
    s.srtt_us = 50'000;
    return s;
}

class RateControllerTest : public ::testing::Test {
protected:
    /// A clear sample `after` the last one; the target it leaves.
    uint32_t clear(Duration after, std::optional<uint64_t> capacity = std::nullopt) {
        now += after;
        rate.on_clear(now, capacity);
        return rate.target();
    }

    uint32_t overuse(Duration after, std::optional<uint64_t> capacity = std::nullopt) {
        now += after;
        rate.on_overuse(now, kRtt, capacity);
        return rate.target();
    }

    Timestamp now{100s};
    RateController rate{kLimits, kLifetime};
};

}  // namespace

// ── PathMonitor ─────────────────────────────────────────────────────────────

TEST(PathMonitor, SaysNothingOfAnInvalidSample) {
    PathMonitor monitor;
    PathStats invalid = sample(8'000'000, false);
    invalid.valid = false;
    invalid.live_expired = 3;
    const PathSignal signal = monitor.update(invalid, Timestamp{1s});
    EXPECT_FALSE(signal.expired);
    EXPECT_FALSE(signal.capacity);
}

TEST(PathMonitor, TakesTheQueueForGrowingOnlyInTwoSamplesRunning) {
    PathMonitor monitor;
    PathStats s = sample(8'000'000);
    s.queued_bytes = 100'000;  // 100 ms at 8 Mbit/s
    EXPECT_EQ(monitor.update(s, Timestamp{1s}).queue_delay, 0ms);
    EXPECT_EQ(monitor.update(s, Timestamp{1100ms}).queue_delay, 100ms);
    s.queued_bytes = 0;
    EXPECT_EQ(monitor.update(s, Timestamp{1200ms}).queue_delay, 0ms);
}

TEST(PathMonitor, ReportsExpiriesSinceTheLastSample) {
    PathMonitor monitor;
    PathStats s = sample(8'000'000);
    s.live_expired = 2;  // since the connection opened
    EXPECT_TRUE(monitor.update(s, Timestamp{1s}).expired);
    EXPECT_FALSE(monitor.update(s, Timestamp{1100ms}).expired);
    s.live_expired = 3;
    EXPECT_TRUE(monitor.update(s, Timestamp{1200ms}).expired);
}

TEST(PathMonitor, KnowsTheCapacityOnlyOfAFullPathAndLeavesOutRepairs) {
    PathMonitor monitor;
    EXPECT_FALSE(monitor.update(sample(10'000'000, true), Timestamp{1s}).capacity);
    // Busy for a moment, as a keyframe keeps it: not full yet.
    EXPECT_FALSE(monitor.update(sample(10'000'000, false), Timestamp{1100ms}).capacity);

    PathStats busy = sample(10'000'000, false);
    busy.live_repair_bytes = 12'500;  // 1 Mbit/s over the 100 ms since
    const PathSignal signal = monitor.update(busy, Timestamp{1200ms});
    ASSERT_TRUE(signal.capacity);
    EXPECT_EQ(*signal.capacity, 9'000'000u);
    EXPECT_EQ(signal.rtt, 50ms);

    // An estimate still growing is still being found.
    PathStats growing = sample(13'000'000, false);
    growing.live_repair_bytes = busy.live_repair_bytes;
    EXPECT_FALSE(monitor.update(growing, Timestamp{1300ms}).capacity);
}

// ── RateController ──────────────────────────────────────────────────────────

TEST(BitrateLimits, Validity) {
    EXPECT_TRUE(is_valid(kLimits));
    EXPECT_TRUE(is_valid(BitrateLimits{1, 1, 1}));
    EXPECT_FALSE(is_valid(BitrateLimits{0, 1, 1}));
    EXPECT_FALSE(is_valid(BitrateLimits{2, 1, 3}));
    EXPECT_FALSE(is_valid(BitrateLimits{1, 3, 2}));
}

TEST_F(RateControllerTest, OverusedByExpiriesOrAQueueOfAQuarterLifetime) {
    PathSignal signal;
    signal.queue_delay = 50ms;
    EXPECT_FALSE(rate.overused(signal));
    signal.queue_delay = 51ms;
    EXPECT_TRUE(rate.overused(signal));
    signal.queue_delay = 0ms;
    signal.expired = true;
    EXPECT_TRUE(rate.overused(signal));
}

TEST_F(RateControllerTest, RampsUpFromTheStartASecondAStep) {
    EXPECT_EQ(rate.target(), 2'000'000u);
    EXPECT_EQ(clear(0s), 2'000'000u);  // the first sample only starts the clock
    EXPECT_EQ(clear(500ms), 2'000'000u);
    EXPECT_EQ(clear(500ms), 3'000'000u);
    EXPECT_EQ(clear(1s), 4'500'000u);
    EXPECT_EQ(clear(1s), 6'750'000u);
    EXPECT_EQ(clear(1s), 8'000'000u);  // the top
    EXPECT_EQ(rate.on_clear(now + 1s, std::nullopt), std::nullopt);
}

TEST_F(RateControllerTest, ClimbsNoHigherThanABusyPathCarries) {
    clear(0s);
    EXPECT_EQ(clear(1s, 3'000'000), 2'700'000u);
    // Within 5% of the target: not worth a keyframe.
    EXPECT_EQ(clear(1s, 2'900'000), 2'700'000u);
}

TEST_F(RateControllerTest, CutsAtOnceToBelowWhatThePathCarries) {
    EXPECT_EQ(overuse(0s, 1'000'000), 850'000u);
    // Frames sent before the cut may still expire: not cut again for a
    // lifetime and a round trip.
    EXPECT_EQ(overuse(200ms), 850'000u);
    EXPECT_EQ(overuse(60ms), 722'500u);  // 85% of the target, the path unknown
}

TEST_F(RateControllerTest, CutsNoMoreThanHalfAtOnce) {
    EXPECT_EQ(overuse(0s, 100'000), 850'000u);  // 85% of half the target
}

TEST_F(RateControllerTest, ProbesCarefullyTowardsWhereThePathFilledUpThenRampsPastIt) {
    clear(0s);
    EXPECT_EQ(overuse(0s, 2'000'000), 1'700'000u);  // a full path: ceiling 2 Mbit/s
    EXPECT_EQ(clear(1900ms), 1'700'000u);           // held for two seconds after a cut
    EXPECT_EQ(clear(100ms), 1'836'000u);            // +8%
    EXPECT_EQ(clear(1s), 1'982'880u);
    EXPECT_EQ(clear(1s), 2'141'510u);
    EXPECT_EQ(clear(1s), 2'312'831u);  // 15% past the ceiling with no trouble
    EXPECT_EQ(clear(1s), 3'469'247u);  // so the path has grown: ramping again
}

TEST_F(RateControllerTest, ClimbsBackQuicklyToBelowTheCeiling) {
    clear(0s);
    EXPECT_EQ(overuse(0s, 2'000'000), 1'700'000u);  // ceiling 2 Mbit/s
    EXPECT_EQ(overuse(1s), 1'445'000u);             // frames expired, the path not full
    EXPECT_EQ(overuse(1s), 1'228'250u);
    EXPECT_EQ(clear(2s), 1'700'000u);  // straight back to where the first cut left it
    EXPECT_EQ(clear(1s), 1'836'000u);  // then +8%
}

TEST_F(RateControllerTest, ACutWithThePathNotKnownToBeFullLeavesNoCeiling) {
    clear(0s);
    EXPECT_EQ(overuse(0s), 1'700'000u);
    EXPECT_EQ(clear(2s), 2'550'000u);  // ramping
}

TEST_F(RateControllerTest, StaysWithinTheLimits) {
    for (int i = 0; i < 20; ++i) overuse(1s);
    EXPECT_EQ(rate.target(), kLimits.min);
    EXPECT_EQ(rate.on_overuse(now + 1s, kRtt, std::nullopt), std::nullopt);  // no change
}

TEST_F(RateControllerTest, RestartGoesBackToTheStartAndForgetsThePath) {
    overuse(0s);
    EXPECT_EQ(rate.restart(), kLimits.start);
    EXPECT_EQ(rate.restart(), std::nullopt);
    clear(0s);
    EXPECT_EQ(clear(1s), 3'000'000u);  // ramping, no ceiling, no hold
}
