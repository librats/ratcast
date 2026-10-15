#include "ratcast/stream/frame_tracker.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr Duration kPatience = 150ms;
constexpr Duration kRetry = 250ms;

EncodedFrame frame(FrameNumber number, bool keyframe, std::initializer_list<FrameNumber> refs) {
    EncodedFrame f;
    f.info.number = number;
    f.info.keyframe = keyframe;
    f.info.references = refs;
    return f;
}

EncodedFrame key(FrameNumber number) { return frame(number, true, {}); }
EncodedFrame delta(FrameNumber number, std::initializer_list<FrameNumber> refs) {
    return frame(number, false, refs);
}
EncodedFrame delta(FrameNumber number) { return delta(number, {number - 1}); }

class FrameTrackerTest : public ::testing::Test {
protected:
    explicit FrameTrackerTest(size_t max_pending = 128)
        : tracker_(FrameTracker::Config{kPatience, kRetry, max_pending}) {
        tracker_.reset(now_);
    }

    void push(EncodedFrame f) { tracker_.push(std::move(f), now_); }
    void wait(Duration d) {
        now_ += d;
        tracker_.update(now_);
    }

    /// Numbers of the frames ready to decode, in the order they came out.
    std::vector<FrameNumber> drain() {
        std::vector<FrameNumber> numbers;
        while (auto f = tracker_.pop()) numbers.push_back(f->info.number);
        return numbers;
    }

    Timestamp now_{10s};
    FrameTracker tracker_;
};

using Frames = std::vector<FrameNumber>;

}  // namespace

TEST_F(FrameTrackerTest, DeliversAStreamInOrder) {
    push(key(0));
    push(delta(1));
    push(delta(2));
    EXPECT_EQ(drain(), (Frames{0, 1, 2}));
    EXPECT_TRUE(tracker_.decodable());
    EXPECT_EQ(tracker_.stats().delivered, 3u);
}

TEST_F(FrameTrackerTest, StartsOnlyAtAKeyframe) {
    push(delta(1));
    push(delta(2));
    EXPECT_EQ(drain(), Frames{});
    EXPECT_FALSE(tracker_.decodable());

    push(key(0));  // overtaken by the frames that reference it
    EXPECT_EQ(drain(), (Frames{0, 1, 2}));
}

TEST_F(FrameTrackerTest, GivesUpFramesWithNothingToStartFrom) {
    push(delta(7));
    wait(kPatience);
    EXPECT_EQ(drain(), Frames{});
    EXPECT_EQ(tracker_.stats().undecodable, 1u);
}

TEST_F(FrameTrackerTest, HoldsFramesBehindAGapUntilItFills) {
    push(key(0));
    push(delta(2));
    push(delta(3));
    EXPECT_EQ(drain(), Frames{0});

    wait(kPatience - 1us);
    push(delta(1));  // repaired late, but within patience
    EXPECT_EQ(drain(), (Frames{1, 2, 3}));
    EXPECT_EQ(tracker_.stats().lost, 0u);
}

TEST_F(FrameTrackerTest, LosingAFrameNothingReferencesBreaksNothing) {
    push(key(0));
    push(delta(2, {0}));  // frame 1 is a top temporal layer: nothing references it
    wait(kPatience - 1us);
    EXPECT_EQ(drain(), Frames{0});

    wait(1us);
    EXPECT_EQ(drain(), Frames{2});
    EXPECT_EQ(tracker_.stats().lost, 1u);
    EXPECT_TRUE(tracker_.decodable());
}

TEST_F(FrameTrackerTest, LosingAReferenceBreaksTheStreamUntilAKeyframe) {
    push(key(0));
    push(delta(2));  // references the lost frame 1
    wait(kPatience);
    EXPECT_EQ(drain(), Frames{0});
    EXPECT_EQ(tracker_.stats().lost, 1u);
    EXPECT_EQ(tracker_.stats().undecodable, 1u);
    EXPECT_FALSE(tracker_.decodable());

    push(delta(3));
    EXPECT_EQ(drain(), Frames{});
    EXPECT_EQ(tracker_.stats().undecodable, 2u);

    push(key(4));
    EXPECT_EQ(drain(), Frames{4});
    EXPECT_TRUE(tracker_.decodable());
}

TEST_F(FrameTrackerTest, AKeyframeDoesNotWaitForTheGapBeforeIt) {
    push(key(0));
    push(delta(2));
    push(key(5));
    EXPECT_EQ(drain(), (Frames{0, 5}));
    EXPECT_EQ(tracker_.stats().superseded, 1u);
    EXPECT_EQ(tracker_.stats().lost, 3u);  // 1, 3 and 4 never came
}

TEST_F(FrameTrackerTest, DropsLateAndDuplicateFrames) {
    push(key(0));
    push(delta(1));
    push(delta(1));
    push(key(0));
    EXPECT_EQ(drain(), (Frames{0, 1}));
    EXPECT_EQ(tracker_.stats().late, 2u);

    push(delta(3));
    push(delta(3));  // a duplicate while waiting
    EXPECT_EQ(tracker_.stats().late, 3u);
}

TEST_F(FrameTrackerTest, ReferencesBeyondTheWindowCountAsLost) {
    push(key(0));
    FrameNumber n = 1;
    for (; n < FrameTracker::kReferenceWindow + 10; ++n) push(delta(n));
    drain();
    push(delta(n, {0}));
    EXPECT_EQ(drain(), Frames{});
    EXPECT_EQ(tracker_.stats().undecodable, 1u);
}

TEST_F(FrameTrackerTest, AsksForKeyframesAtMostOncePerRetry) {
    // Subscribing counted as the first request.
    EXPECT_FALSE(tracker_.want_keyframe(now_));
    EXPECT_TRUE(tracker_.want_keyframe(now_ + kRetry));
    EXPECT_FALSE(tracker_.want_keyframe(now_ + kRetry + 1ms));
    EXPECT_TRUE(tracker_.want_keyframe(now_ + 2 * kRetry));

    push(key(0));
    EXPECT_FALSE(tracker_.want_keyframe(now_ + 10 * kRetry));
    EXPECT_EQ(tracker_.stats().keyframe_requests, 2u);
}

TEST_F(FrameTrackerTest, ResetForgetsEverything) {
    push(key(0));
    push(delta(2));
    tracker_.reset(now_);
    EXPECT_EQ(drain(), Frames{});
    EXPECT_FALSE(tracker_.decodable());

    push(key(0));  // a number from before the reset is new again
    EXPECT_EQ(drain(), Frames{0});
}

namespace {
class SmallFrameTrackerTest : public FrameTrackerTest {
protected:
    SmallFrameTrackerTest() : FrameTrackerTest(4) {}
};
}  // namespace

TEST_F(SmallFrameTrackerTest, AFullQueueGivesUpTheOldestGap) {
    push(key(0));
    for (FrameNumber n = 2; n <= 5; ++n) push(delta(n, {0}));  // waiting for 1
    EXPECT_EQ(drain(), Frames{0});

    push(delta(6, {0}));  // no room left: frame 1 is given up
    EXPECT_EQ(drain(), (Frames{2, 3, 4, 5, 6}));
    EXPECT_EQ(tracker_.stats().lost, 1u);
}
