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

    uint32_t overuse(Duration after, std::optional<uint64_t> capacity = std::nullopt,
                     std::optional<uint64_t> delivered = std::nullopt) {
        now += after;
        PathSignal path;
        path.rtt = kRtt;
        path.capacity = capacity;
        path.delivered = delivered;
        rate.on_overuse(now, path);
        return rate.target();
    }

    Timestamp now{100s};
    RateController rate{kLimits, kLifetime};
};

}  // namespace

// ── PathMonitor ─────────────────────────────────────────────────────────────

TEST(PathShare, IsAPartOfWhatThePathCarriesAndAllElseOfThePath) {
    PathSignal path;
    path.expired = true;
    path.queue_delay = 30ms;
    path.rtt = 50ms;
    path.capacity = 5'000'000;
    path.delivered = 4'000'000;
    const PathSignal part = share_of(path, 0.2);
    EXPECT_EQ(part.capacity, 1'000'000u);
    EXPECT_EQ(part.delivered, 800'000u);
    EXPECT_TRUE(part.expired);
    EXPECT_EQ(part.queue_delay, 30ms);
    EXPECT_EQ(part.rtt, 50ms);
    EXPECT_FALSE(share_of(PathSignal{}, 0.5).capacity) << "unknown stays unknown";
}

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

TEST(PathMonitor, MeasuresWhatABusyPathDeliveredLessRepairs) {
    PathMonitor monitor;
    PathStats s = sample(10'000'000, true);
    s.bytes_delivered = 1'000'000;
    monitor.update(s, Timestamp{1s});
    s.bytes_delivered += 50'000;  // 4 Mbit/s over 100 ms, but not busy all along
    s.app_limited = false;
    EXPECT_FALSE(monitor.update(s, Timestamp{1100ms}).delivered);

    s.bytes_delivered += 50'000;
    s.live_repair_bytes = 2'500;  // 0.2 Mbit/s of it repairs
    const PathSignal signal = monitor.update(s, Timestamp{1200ms});
    ASSERT_TRUE(signal.delivered);
    EXPECT_EQ(*signal.delivered, 3'800'000u);
    // The estimate remembers the path wider; it is no longer growing, so the
    // path is full all the same.
    EXPECT_EQ(signal.capacity, 9'800'000u);
}

TEST(PathMonitor, SaysWhatAnyPathCarriedBusyOrNot) {
    PathMonitor monitor;
    PathStats s = sample(10'000'000, true);
    EXPECT_FALSE(monitor.update(s, Timestamp{1s}).carried);  // nothing to count from

    s.bytes_delivered = 25'000;  // 2 Mbit/s over 100 ms
    s.live_repair_bytes = 2'500;
    const PathSignal signal = monitor.update(s, Timestamp{1100ms});
    EXPECT_EQ(signal.carried, 1'800'000u);
    EXPECT_EQ(signal.since, 100ms);
    EXPECT_FALSE(signal.delivered);  // idle: no measure of the path itself
}

TEST(PathMonitor, SaysHowFarTheRoundTripRoseOverItsLeastOfTenSeconds) {
    PathMonitor monitor;
    PathStats s = sample(10'000'000, true);
    const auto rise = [&](uint32_t rtt_ms, Timestamp at) {
        s.srtt_us = rtt_ms * 1000;
        return monitor.update(s, at).queue_rise;
    };
    EXPECT_EQ(rise(80, Timestamp{1s}), 0ms);  // a jittery path: always above its least ever
    EXPECT_EQ(rise(70, Timestamp{2s}), 0ms);
    EXPECT_EQ(rise(110, Timestamp{3s}), 40ms);
    EXPECT_EQ(rise(110, Timestamp{12s}), 40ms);     // 70 ms is still within ten seconds
    EXPECT_EQ(rise(110, Timestamp{12500ms}), 0ms);  // no longer: the queue stands
}

TEST(PathMonitor, SaysWhatAPathCarriedOverTheLastSecond) {
    PathMonitor monitor;
    PathStats s = sample(10'000'000, true);
    monitor.update(s, Timestamp{1s});
    for (int i = 1; i <= 15; ++i) {
        // 4 Mbit/s for a second, then nothing acknowledged for half a second.
        if (i <= 10) s.bytes_delivered += 50'000;
        const PathSignal signal = monitor.update(s, Timestamp{1s} + i * 100ms);
        if (i == 10) {
            EXPECT_EQ(signal.carried, 4'000'000u);
        }
        if (i == 15) {
            EXPECT_EQ(signal.carried, 2'000'000u);  // over the last second
        }
    }
}

TEST(PathMonitor, SaysNothingOfThePathWhileTheTransportIsStillFindingIt) {
    PathMonitor monitor;
    PathStats s = sample(1'000'000, false);
    s.pacing_bps = 2'770'000;  // BBR's Startup
    s.queued_bytes = 50'000;   // 400 ms at what it has found so far
    s.live_expired = 1;
    monitor.update(s, Timestamp{1s});
    s.bytes_delivered = 12'500;
    const PathSignal signal = monitor.update(s, Timestamp{1100ms});
    EXPECT_TRUE(signal.starting);
    EXPECT_FALSE(signal.capacity);
    EXPECT_FALSE(signal.delivered);

    s.pacing_bps = 1'250'000;  // probing past Startup
    EXPECT_FALSE(monitor.update(s, Timestamp{1200ms}).starting);
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

TEST_F(RateControllerTest, NotOverusedWhileTheTransportIsStillFindingThePath) {
    PathSignal signal;
    signal.starting = true;
    signal.expired = true;
    signal.queue_delay = 1s;
    EXPECT_FALSE(rate.overused(signal));
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

TEST_F(RateControllerTest, CutsByWhatABusyPathDeliveredWhereItsEstimateLags) {
    clear(0s);
    // The estimate still remembers 10 Mbit/s; 1.2 got through.
    EXPECT_EQ(overuse(0s, 10'000'000, 1'200'000), 1'020'000u);
    EXPECT_EQ(clear(2s), 1'101'600u);  // careful: the path filled up at 1.2 Mbit/s
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

TEST_F(RateControllerTest, ProbesOnFromJustShortOfWhereACutLeftIt) {
    clear(0s);
    // 85% of the ceiling, rounded down: a hair short of it, which is no raise.
    EXPECT_EQ(overuse(0s, 1'000'005), 850'004u);
    EXPECT_EQ(clear(2s), 918'004u);  // +8%
}

TEST_F(RateControllerTest, ACutWithThePathNotKnownToBeFullLeavesNoCeiling) {
    clear(0s);
    EXPECT_EQ(overuse(0s), 1'700'000u);
    EXPECT_EQ(clear(2s), 2'550'000u);  // ramping
}

TEST_F(RateControllerTest, StaysWithinTheLimits) {
    for (int i = 0; i < 20; ++i) overuse(1s);
    EXPECT_EQ(rate.target(), kLimits.min);
    EXPECT_EQ(rate.on_overuse(now + 1s, PathSignal{}), std::nullopt);  // no change
}

TEST_F(RateControllerTest, RestartGoesBackToTheStartAndForgetsThePath) {
    overuse(0s);
    EXPECT_EQ(rate.restart(), kLimits.start);
    EXPECT_EQ(rate.restart(), std::nullopt);
    clear(0s);
    EXPECT_EQ(clear(1s), 3'000'000u);  // ramping, no ceiling, no hold
}

TEST_F(RateControllerTest, ALimitComesDownAtOnceToWhereARaiseWouldStop) {
    EXPECT_EQ(rate.limit(2'000'000), 1'800'000u);
    EXPECT_EQ(rate.limit(2'000'000), std::nullopt);   // already below
    EXPECT_EQ(rate.limit(10'000'000), std::nullopt);  // never up
    EXPECT_EQ(rate.target(), 1'800'000u);
}
