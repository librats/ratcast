#include "ratcast/session/common_delay.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

PeerId peer(uint8_t seed) {
    std::array<uint8_t, 32> key{};
    key.fill(seed);
    return PeerId::from_public_key(key.data(), key.size());
}

class CommonDelayTest : public ::testing::Test {
protected:
    bool need(uint8_t who, std::optional<Duration> latency, Duration after = 0s) {
        now += after;
        return common.need(peer(who), latency, now);
    }

    bool update(Duration after) {
        now += after;
        return common.update(now);
    }

    Timestamp now{100s};
    CommonDelay common;
};

}  // namespace

TEST_F(CommonDelayTest, AViewerAloneHasNobodyToBeInStepWith) {
    EXPECT_FALSE(need(1, 80ms));
    EXPECT_FALSE(common.delay());
    EXPECT_TRUE(common.plays(peer(1)));
}

TEST_F(CommonDelayTest, IsTheMostAnyViewerNeedsAndAMargin) {
    need(1, 80ms);
    EXPECT_TRUE(need(2, 150ms));
    EXPECT_EQ(common.delay(), 150ms + CommonDelay::kMargin);
    EXPECT_FALSE(need(3, 100ms));  // within it
    auto viewers = common.viewers();
    std::sort(viewers.begin(), viewers.end());
    auto expected = std::vector{peer(1), peer(2), peer(3)};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(viewers, expected);
}

TEST_F(CommonDelayTest, RisesAtOnce) {
    need(1, 80ms);
    need(2, 150ms);
    EXPECT_TRUE(need(1, 200ms, 1s));
    EXPECT_EQ(common.delay(), 200ms + CommonDelay::kMargin);
}

TEST_F(CommonDelayTest, ComesDownOnceTheNeedsStayedLower) {
    need(1, 80ms);
    need(2, 150ms);
    EXPECT_FALSE(need(2, 90ms, 1s));
    EXPECT_FALSE(update(CommonDelay::kLowerAfter - 1ms));
    EXPECT_EQ(common.delay(), 150ms + CommonDelay::kMargin);
    EXPECT_TRUE(update(1ms));
    EXPECT_EQ(common.delay(), 90ms + CommonDelay::kMargin);
}

TEST_F(CommonDelayTest, ALittleLessIsNoReasonToComeDown) {
    need(1, 80ms);
    need(2, 150ms);
    need(2, 141ms, 1s);
    EXPECT_FALSE(update(1min));
    EXPECT_EQ(common.delay(), 150ms + CommonDelay::kMargin);
}

TEST_F(CommonDelayTest, ANeedBackUpForAMomentStartsTheWaitOver) {
    need(1, 80ms);
    need(2, 150ms);
    need(2, 90ms, 1s);
    update(3s);
    need(2, 145ms, 100ms);
    need(2, 90ms, 100ms);
    EXPECT_FALSE(update(CommonDelay::kLowerAfter - 1ms));
    EXPECT_TRUE(update(1ms));
}

TEST_F(CommonDelayTest, AViewerLeavingTheOtherAloneLeavesNoDelay) {
    need(1, 80ms);
    need(2, 150ms);
    EXPECT_TRUE(need(2, std::nullopt));
    EXPECT_FALSE(common.delay());
    EXPECT_FALSE(common.plays(peer(2)));

    // Back, it is the most needed again at once.
    EXPECT_TRUE(need(2, 150ms, 1s));
    EXPECT_EQ(common.delay(), 150ms + CommonDelay::kMargin);
}
