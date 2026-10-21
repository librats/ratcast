// The stamped TS probe and its meter: what it counts of a feed that came
// whole, late, short, twice or out of order.

#include "ratcast/client/ts_probe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

/// Packets numbered from 0, as sent at `sent`.
class Probe {
public:
    explicit Probe(size_t count, Timestamp sent = Timestamp{}) : bytes_(count * kTsPacketSize) {
        TsProbeWriter writer;
        writer.fill(bytes_, sent);
    }
    std::span<const uint8_t> packet(size_t n) const {
        return std::span(bytes_).subspan(n * kTsPacketSize, kTsPacketSize);
    }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

class TsProbeTest : public ::testing::Test {
protected:
    void SetUp() override { meter_.start(); }

    /// Packets `numbers` of `probe`, received now, in that order.
    void receive(const Probe& probe, std::initializer_list<size_t> numbers) {
        for (size_t n : numbers) meter_.take(probe.packet(n), clock_.now());
    }

    ManualClock clock_;
    TsProbeMeter meter_{clock_};
};

}  // namespace

TEST_F(TsProbeTest, MeasuresAFeedThatCameWhole) {
    const Probe probe(100, clock_.now());
    clock_.advance(30ms);
    // Cut wherever, after bytes that are no packet.
    meter_.take(std::vector<uint8_t>{1, 2, 3}, clock_.now());
    for (size_t at = 0; at < probe.bytes().size(); at += 1000) {
        meter_.take(
            std::span(probe.bytes()).subspan(at, std::min<size_t>(1000, probe.bytes().size() - at)),
            clock_.now());
    }
    const TsProbeStats total = meter_.total();
    EXPECT_EQ(total.packets, 100u);
    EXPECT_EQ(total.lost, 0u);
    EXPECT_EQ(total.duplicates, 0u);
    EXPECT_EQ(total.reordered, 0u);
    EXPECT_EQ(total.discontinuities, 0u);
    EXPECT_EQ(total.foreign, 3u);
    EXPECT_EQ(total.bytes, probe.bytes().size() + 3);
    EXPECT_EQ(total.first_packet, 30ms);
    EXPECT_EQ(total.latency.count(), 100u);
    EXPECT_EQ(total.latency.max(), 30ms);
}

TEST_F(TsProbeTest, CountsWhatWasLostDoubledAndOutOfOrder) {
    const Probe probe(40);
    // 0..9, 12 before 11, 10 never; 20 twice; 21..29 lost: a burst; 30..39.
    receive(probe, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 11, 13, 14, 15, 16, 17, 18, 19, 20, 20});
    receive(probe, {30, 31, 32, 33, 34, 35, 36, 37, 38, 39});
    const TsProbeStats total = meter_.total();
    EXPECT_EQ(total.packets, 31u);
    EXPECT_EQ(total.lost, 10u);  // 10 and 21..29
    EXPECT_EQ(total.duplicates, 1u);
    EXPECT_EQ(total.reordered, 1u);
    // A decoder sees the jumps at 12, back to 11, on to 13, and from 20 to 30.
    EXPECT_EQ(total.discontinuities, 4u);
    EXPECT_EQ(total.latency.count(), 30u);  // the copy is not measured again
}

TEST_F(TsProbeTest, LossCountsFromTheFirstPacketSeen) {
    const Probe probe(20);
    receive(probe, {5, 6, 4, 7});  // joined at 5; 4 came late, it is no loss
    EXPECT_EQ(meter_.total().lost, 0u);
    EXPECT_EQ(meter_.total().reordered, 1u);
}

TEST_F(TsProbeTest, ForgetsNumbersFarBehind) {
    // A jump past the window: what it held is forgotten, so the old numbers'
    // places do not read as seen for the new ones.
    std::vector<uint8_t> packet(kTsPacketSize);
    TsProbeWriter writer;
    writer.fill(packet, clock_.now());  // 0
    meter_.take(packet, clock_.now());
    for (uint64_t n = 1; n < 70'000; ++n) writer.fill(packet, clock_.now());
    meter_.take(packet, clock_.now());  // 69'999
    EXPECT_EQ(meter_.total().duplicates, 0u);
    EXPECT_EQ(meter_.total().lost, 69'998u);
    // And 0 is too far behind to tell: dropped as a copy.
    writer = {};
    writer.fill(packet, clock_.now());
    meter_.take(packet, clock_.now());
    EXPECT_EQ(meter_.total().duplicates, 1u);
}

TEST_F(TsProbeTest, IntervalsStartAnew) {
    const Probe probe(10);
    clock_.advance(5ms);
    receive(probe, {0, 1, 2});
    clock_.advance(40ms);
    receive(probe, {3});
    const TsProbeStats first = meter_.take_interval();
    EXPECT_EQ(first.packets, 4u);
    EXPECT_EQ(first.longest_gap, 40ms);
    receive(probe, {4, 5});
    const TsProbeStats second = meter_.take_interval();
    EXPECT_EQ(second.packets, 2u);
    EXPECT_EQ(second.longest_gap, 0ms);
    EXPECT_EQ(meter_.total().packets, 6u);
    EXPECT_EQ(meter_.total().first_packet, 5ms);
    meter_.start();
    EXPECT_EQ(meter_.total().packets, 0u);
    EXPECT_FALSE(meter_.total().first_packet);
}

TEST_F(TsProbeTest, OtherPidsAreNoProbe) {
    Probe probe(1);
    std::vector<uint8_t> other(probe.bytes());
    other[2] = 0x42;
    meter_.take(other, clock_.now());
    EXPECT_EQ(meter_.total().packets, 0u);
    EXPECT_EQ(meter_.total().foreign, kTsPacketSize);
}
