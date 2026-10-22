#include "ratcast/client/playback_meter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

Duration us(int64_t n) { return Duration{n}; }

/// The exact percentile of `values`, as LatencyHistogram defines it.
Duration exact_percentile(std::vector<Duration> values, double p) {
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<size_t>(std::max(1.0, std::ceil(p * double(values.size()))));
    return values[rank - 1];
}

}  // namespace

TEST(LatencyHistogram, SmallValuesAreExact) {
    LatencyHistogram h;
    for (int64_t v = 0; v < 64; ++v) h.add(us(v));
    EXPECT_EQ(h.count(), 64u);
    EXPECT_EQ(h.percentile(0.5), us(31));
    EXPECT_EQ(h.percentile(1.0), us(63));
    EXPECT_EQ(h.percentile(0.0), us(0));
    EXPECT_EQ(h.max(), us(63));
    EXPECT_EQ(h.mean(), us(31));  // 31.5, rounded down
}

TEST(LatencyHistogram, PercentilesAreWithinTheBucketWidth) {
    std::mt19937_64 random(7);
    std::lognormal_distribution<double> latency(std::log(20'000.0), 0.6);  // around 20 ms
    LatencyHistogram h;
    std::vector<Duration> values;
    for (int i = 0; i < 20'000; ++i) {
        const Duration v = us(static_cast<int64_t>(latency(random)));
        h.add(v);
        values.push_back(v);
    }
    for (double p : {0.01, 0.5, 0.9, 0.95, 0.99, 0.999, 1.0}) {
        const double exact = double(exact_percentile(values, p).count());
        const double got = double(h.percentile(p).count());
        // Never below the true value, and less than a bucket (1/32) above it.
        EXPECT_GE(got, exact) << p;
        EXPECT_LE(got, exact * (1 + 1.0 / 32)) << p;
    }
    EXPECT_EQ(h.max(), *std::max_element(values.begin(), values.end()));
}

TEST(LatencyHistogram, ExtremesAreHeld) {
    LatencyHistogram h;
    EXPECT_EQ(h.percentile(0.5), Duration{});
    EXPECT_EQ(h.mean(), Duration{});
    h.add(us(-5));                   // a clock that stepped back: counted as zero
    h.add(std::chrono::hours(100));  // past the last bucket: kept there
    EXPECT_EQ(h.count(), 2u);
    EXPECT_EQ(h.percentile(0.5), Duration{});
    EXPECT_EQ(h.percentile(1.0), std::chrono::hours(100));  // the maximum is exact
    h.clear();
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.max(), Duration{});
}

TEST(PlaybackMeter, MeasuresTheWaitStallsAndLatency) {
    ManualClock clock;
    PlaybackMeter meter(clock, 100ms);
    meter.start();

    clock.advance(700ms);  // joining
    meter.picture(30ms);
    for (int i = 0; i < 9; ++i) {
        clock.advance(20ms);
        meter.picture(std::nullopt);  // no capture time known
    }
    clock.advance(250ms);  // the picture stands still
    meter.picture(40ms);
    clock.advance(20ms);
    meter.picture(20ms);

    const PlaybackStats total = meter.total();
    EXPECT_EQ(total.pictures, 12u);
    EXPECT_EQ(total.first_picture, 700ms);
    EXPECT_EQ(total.stalls, 1u);
    EXPECT_EQ(total.stalled, 250ms);
    EXPECT_EQ(total.longest_gap, 250ms);
    EXPECT_EQ(total.latency.count(), 3u);
    EXPECT_EQ(total.latency.mean(), 30ms);
    EXPECT_EQ(total.latency.max(), 40ms);
}

TEST(PlaybackMeter, IntervalsStartAfresh) {
    ManualClock clock;
    PlaybackMeter meter(clock, 100ms);
    meter.start();
    meter.picture(10ms);
    clock.advance(150ms);
    meter.picture(10ms);

    const PlaybackStats first = meter.take_interval();
    EXPECT_EQ(first.pictures, 2u);
    EXPECT_EQ(first.stalls, 1u);
    EXPECT_FALSE(first.first_picture);  // a total's

    clock.advance(20ms);  // a gap that spans the intervals counts in the next
    meter.picture(5ms);
    const PlaybackStats second = meter.take_interval();
    EXPECT_EQ(second.pictures, 1u);
    EXPECT_EQ(second.stalls, 0u);
    EXPECT_EQ(second.longest_gap, 20ms);
    EXPECT_EQ(second.latency.max(), 5ms);
    EXPECT_EQ(meter.take_interval().pictures, 0u);

    EXPECT_EQ(meter.total().pictures, 3u);
    meter.start();  // and start() clears everything
    EXPECT_EQ(meter.total().pictures, 0u);
    EXPECT_FALSE(meter.total().first_picture);
}

TEST(LatencyHistogram, SinceAnEarlierCopyHoldsWhatWasAddedAfter) {
    LatencyHistogram h;
    for (int64_t v : {500, 40'000, 1'000}) h.add(us(v));
    const LatencyHistogram earlier = h;
    for (int64_t v : {2'000, 3'000}) h.add(us(v));

    const LatencyHistogram added = h.since(earlier);
    EXPECT_EQ(added.count(), 2u);
    EXPECT_EQ(added.mean(), us(2'500));
    EXPECT_GE(added.percentile(0.5), us(2'000));
    EXPECT_LE(added.percentile(0.5), us(2'000 + 2'000 / 32));
    // The largest is not the whole one's: the top of its bucket.
    EXPECT_GE(added.max(), us(3'000));
    EXPECT_LE(added.max(), us(3'000 + 3'000 / 32));

    h.add(us(90'000));
    EXPECT_EQ(h.since(earlier).max(), us(90'000)) << "a new largest is exact";
    EXPECT_EQ(earlier.since(h).count(), earlier.count()) << "one that started over: all of it";
}

TEST(FramePace, MeasuresTheIntervalsAndHowMuchTheyChange) {
    FramePace pace;
    Timestamp at{};
    pace.picture(at);
    EXPECT_EQ(pace.intervals().count(), 0u);
    for (int64_t step : {10'000, 30'000, 10'000, 30'000}) {
        at += us(step);
        pace.picture(at);
    }
    EXPECT_EQ(pace.intervals().count(), 4u);
    EXPECT_EQ(pace.intervals().max(), us(30'000));
    EXPECT_EQ(pace.jitter(), us(20'000));

    // Starting over leaves out the gap before the next picture.
    const FramePace earlier = pace;
    pace.restart();
    at += std::chrono::seconds(5);
    pace.picture(at);
    for (int i = 0; i < 3; ++i) {
        at += us(16'000);
        pace.picture(at);
    }
    const FramePace added = pace.since(earlier);
    EXPECT_EQ(added.intervals().count(), 3u);
    EXPECT_GE(added.intervals().max(), us(16'000));
    EXPECT_LE(added.intervals().max(), us(16'000 + 16'000 / 32));
    EXPECT_EQ(added.jitter(), Duration{});
}
