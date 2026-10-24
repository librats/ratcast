#include "ratcast/session/shared_uplink.h"

#include <gtest/gtest.h>

#include <array>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

PeerId peer(uint8_t seed) {
    std::array<uint8_t, 32> key{};
    key.fill(seed);
    return PeerId::from_public_key(key.data(), key.size());
}

/// A sample of a path that carried `bits` per second.
PathSignal carrying(uint64_t bits) {
    PathSignal signal;
    signal.carried = bits;
    return signal;
}

class SharedUplinkTest : public ::testing::Test {
protected:
    /// `who` says its path is overused, one of `paths`, `after` the last word.
    bool overused(uint8_t who, uint64_t carried, Duration after = 0s, size_t paths = 3) {
        now += after;
        return uplink.update(peer(who), carrying(carried), true, paths, now);
    }

    /// `who`'s round trip is `rise` over its least, its path not overused.
    bool rising(uint8_t who, Duration rise, uint64_t carried, Duration after, size_t paths = 2) {
        now += after;
        PathSignal signal = carrying(carried);
        signal.queue_rise = rise;
        return uplink.update(peer(who), signal, false, paths, now);
    }

    Timestamp now{100s};
    SharedUplink uplink;
};

}  // namespace

TEST_F(SharedUplinkTest, IsFullWhenEveryPathWithCopiesIsOverusedAtOnce) {
    overused(1, 2'000'000);
    overused(2, 1'900'000, 100ms);
    EXPECT_FALSE(uplink.capacity());  // the third path is fine: these two are their own
    overused(3, 2'100'000, 200ms);
    EXPECT_EQ(uplink.capacity(), 6'000'000u);  // what they carried together
}

TEST_F(SharedUplinkTest, AQueueBuildingBeyondEveryPathIsTheUplinkFilling) {
    for (uint8_t who : {1, 2}) rising(who, 5ms, 2'500'000, 0s);
    EXPECT_FALSE(rising(1, 30ms, 2'500'000, 100ms));  // a keyframe to each lifts it once
    EXPECT_FALSE(rising(2, 30ms, 2'500'000, 0s));
    EXPECT_FALSE(rising(1, 40ms, 2'800'000, 100ms));  // two samples running
    EXPECT_TRUE(rising(2, 40ms, 2'800'000, 0s));
    EXPECT_EQ(uplink.capacity(), 5'600'000u);
}

TEST_F(SharedUplinkTest, APathJustBegunNeedNotSay) {
    // A third viewer's path, its keyframe on the way: the other two fill the
    // uplink before it has said anything.
    uplink.update(peer(3), carrying(100'000), false, 3, now);
    EXPECT_FALSE(overused(1, 2'900'000, 500ms, 3));
    EXPECT_TRUE(overused(2, 3'000'000, 0s, 3));
    EXPECT_EQ(uplink.capacity(), 6'000'000u);

    // A second on, it has had its say.
    EXPECT_FALSE(overused(1, 2'900'000, 1s, 3));
    EXPECT_FALSE(overused(2, 3'000'000, 0s, 3));
}

TEST_F(SharedUplinkTest, OnceKnownTheCopiesBelowTheirPartLeaveIt) {
    overused(1, 3'000'000, 0s, 2);
    overused(2, 3'000'000, 0s, 2);  // 6 Mbit/s: the copies' part is 5.1
    for (uint8_t who : {1, 2}) rising(who, 40ms, 2'000'000, 100ms);
    for (uint8_t who : {1, 2}) EXPECT_FALSE(rising(who, 40ms, 2'000'000, 100ms));
    EXPECT_EQ(uplink.capacity(), 6'000'000u);

    // Past their part, climbing past it: the queue is theirs.
    for (uint8_t who : {1, 2}) rising(who, 40ms, 2'700'000, 100ms);
    EXPECT_TRUE(rising(1, 40ms, 2'700'000, 100ms));
    EXPECT_EQ(uplink.capacity(), 6'000'000u);  // found full again, no narrower
}

TEST_F(SharedUplinkTest, OverusesTooFarApartAreEachThePathsOwn) {
    overused(1, 2'000'000, 0s, 2);
    overused(2, 2'000'000, 1100ms, 2);
    EXPECT_FALSE(uplink.capacity());
}

TEST_F(SharedUplinkTest, OnePathTellsNothingOfTheUplink) {
    overused(1, 2'000'000, 0s, 1);
    overused(1, 2'000'000, 100ms, 1);
    EXPECT_FALSE(uplink.capacity());
}

TEST_F(SharedUplinkTest, IsTheMostItWasFoundToCarryForAMinute) {
    overused(1, 3'000'000, 0s, 2);
    overused(2, 3'000'000, 0s, 2);
    EXPECT_EQ(uplink.capacity(), 6'000'000u);

    // Somebody else's upload for a while, the copies within their part:
    // theirs to make way for, path by path.
    overused(1, 2'000'000, 10s, 2);
    EXPECT_FALSE(overused(2, 2'000'000, 0s, 2));
    // Past their part, the paths sampled as the copies come down: no narrower.
    for (uint8_t who : {1, 2}) overused(who, 2'800'000, 100ms, 2);
    EXPECT_TRUE(overused(1, 2'800'000, 100ms, 2));
    EXPECT_EQ(uplink.capacity(), 6'000'000u);
    // Wider, as it was found after a minute forgotten.
    overused(1, 3'300'000, 100ms, 2);
    EXPECT_TRUE(overused(2, 3'300'000, 0s, 2));
    EXPECT_EQ(uplink.capacity(), 6'600'000u);

    uplink.update(peer(1), carrying(3'000'000), false, 2, now + SharedUplink::kMemory);
    EXPECT_EQ(uplink.capacity(), 6'600'000u);
    uplink.update(peer(1), carrying(3'000'000), false, 2, now + SharedUplink::kMemory + 1ms);
    EXPECT_FALSE(uplink.capacity());
}

TEST_F(SharedUplinkTest, NotWhenThisNodeStalled) {
    // The whole machine stopped for 600 ms: every path's frames expired
    // waiting for it, and little was carried.
    PathSignal late = carrying(500'000);
    late.since = 600ms;
    uplink.update(peer(1), late, true, 2, now);
    overused(2, 500'000, 100ms, 2);
    overused(1, 500'000, 100ms, 2);
    EXPECT_FALSE(uplink.capacity());

    // A second on, the paths are their own again.
    overused(1, 3'000'000, 1s, 2);
    overused(2, 3'000'000, 100ms, 2);
    EXPECT_EQ(uplink.capacity(), 6'000'000u);
}

TEST_F(SharedUplinkTest, APathGoneNoLongerCounts) {
    overused(1, 3'000'000, 0s, 2);
    uplink.forget(peer(1));
    overused(2, 3'000'000, 100ms, 2);
    EXPECT_FALSE(uplink.capacity());
}
