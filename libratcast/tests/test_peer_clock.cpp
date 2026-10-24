#include "ratcast/session/peer_clock.h"

#include <gtest/gtest.h>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

/// The peer's clock runs this far behind this one.
constexpr Duration kBehind = 1000s;

class PeerClockTest : public ::testing::Test {
protected:
    /// A probe sent now, `out` on its way there and `back` on its way back.
    void probe(Duration out, Duration back) {
        const Timestamp sent = now;
        const Timestamp at = now + out - kBehind;
        now += out + back;
        clock.add(sent, at, now);
    }

    Timestamp now{100s};
    PeerClock clock;
};

}  // namespace

TEST_F(PeerClockTest, NothingBeforeAProbeCameBack) {
    EXPECT_FALSE(clock.offset());
    EXPECT_FALSE(clock.round_trip());
}

TEST_F(PeerClockTest, ARoundTripAsLongBothWaysTellsTheClock) {
    probe(20ms, 20ms);
    EXPECT_EQ(clock.offset(), kBehind);
    EXPECT_EQ(clock.round_trip(), 40ms);
}

TEST_F(PeerClockTest, TheQuickestRoundTripTellsItBest) {
    probe(20ms, 80ms);  // waited 60 ms in a queue on the way back
    EXPECT_EQ(clock.offset(), kBehind + 30ms);
    probe(20ms, 20ms);
    EXPECT_EQ(clock.offset(), kBehind);
    probe(70ms, 20ms);  // and on the way there
    EXPECT_EQ(clock.offset(), kBehind);
    EXPECT_EQ(clock.round_trip(), 40ms);
}

TEST_F(PeerClockTest, ARoundTripCountsForItsWindow) {
    probe(20ms, 20ms);
    now += PeerClock::kWindow - 1s;
    probe(20ms, 60ms);
    EXPECT_EQ(clock.offset(), kBehind);
    // The quick one is ten seconds old: the clocks may have drifted since.
    now += 1s;
    probe(20ms, 80ms);
    EXPECT_EQ(clock.offset(), kBehind + 20ms);
    EXPECT_EQ(clock.round_trip(), 80ms);
}

TEST_F(PeerClockTest, AnEchoOfNoProbeOfThisClocksIsNoRoundTrip) {
    clock.add(now, now - kBehind, now - 1ms);
    EXPECT_FALSE(clock.offset());
}
