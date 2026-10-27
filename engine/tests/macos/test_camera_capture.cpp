// Cameras through AVFoundation: listed with nodes that hold from one listing
// to the next; captured into 4:2:0 at even sizes no larger than asked, at the
// time they were taken; a camera that is not there ending its capture. Those
// that need a camera skip where there is none (CI's runner), or where the
// user has not let this process use it.

#include "ratcast/engine/camera_capture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

/// What a capture handed out, for the test thread to wait on.
class Seen {
public:
    VideoSource::Sink sink() {
        return [this](VideoFrame picture) {
            std::lock_guard lock(mutex_);
            pictures_.push_back(std::move(picture));
            changed_.notify_all();
        };
    }
    VideoSource::EndHandler ended() {
        return [this](Result<void> outcome) {
            std::lock_guard lock(mutex_);
            outcome_ = outcome;
            changed_.notify_all();
        };
    }

    std::vector<VideoFrame> wait_for(size_t count) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, 5s, [&] { return pictures_.size() >= count; });
        return pictures_;
    }
    std::optional<Result<void>> wait_for_end() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, 5s, [&] { return outcome_.has_value(); });
        return outcome_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<VideoFrame> pictures_;
    std::optional<Result<void>> outcome_;
};

}  // namespace

TEST(MacCameraTest, AreListedWithNodesThatHold) {
    const Result<std::vector<CameraInfo>> cameras = list_cameras();
    ASSERT_TRUE(cameras);
    const Result<std::vector<CameraInfo>> again = list_cameras();
    ASSERT_TRUE(again);
    ASSERT_EQ(again->size(), cameras->size());
    std::set<uint32_t> nodes;
    for (size_t i = 0; i < cameras->size(); ++i) {
        const CameraInfo& camera = (*cameras)[i];
        EXPECT_NE(camera.node, 0u);
        EXPECT_FALSE(camera.id.empty());
        EXPECT_FALSE(camera.name.empty());
        EXPECT_TRUE(nodes.insert(camera.node).second) << "two cameras of one node";
        EXPECT_EQ((*again)[i].node, camera.node);
    }
}

TEST(MacCameraTest, OneThatIsNotThereEnds) {
    const Result<std::vector<CameraInfo>> cameras = list_cameras();
    ASSERT_TRUE(cameras);
    uint32_t node = 0x5EED;
    while (std::ranges::find(*cameras, node, &CameraInfo::node) != cameras->end()) ++node;

    Result<std::unique_ptr<VideoSource>> camera = open_camera({.node = node});
    ASSERT_TRUE(camera);
    Seen seen;
    ASSERT_TRUE((*camera)->start(seen.sink(), seen.ended()));
    const std::optional<Result<void>> outcome = seen.wait_for_end();
    ASSERT_TRUE(outcome);
    ASSERT_FALSE(*outcome);
    EXPECT_EQ(outcome->error(), Errc::Unavailable);
    (*camera)->stop();
}

TEST(MacCameraTest, CapturesInto420AtEvenSizes) {
    for (const PixelFormat format : {PixelFormat::NV12, PixelFormat::I420}) {
        Result<std::unique_ptr<VideoSource>> camera =
            open_camera({.format = format, .max_width = 640, .max_height = 480});
        if (!camera) GTEST_SKIP() << "no camera here";
        Seen seen;
        if (!(*camera)->start(seen.sink(), seen.ended()))
            GTEST_SKIP() << "the camera is not ours to use";
        const std::vector<VideoFrame> pictures = seen.wait_for(3);
        (*camera)->stop();
        ASSERT_GE(pictures.size(), 3u) << int(format);
        for (const VideoFrame& picture : pictures) {
            EXPECT_EQ(picture.format(), format);
            EXPECT_EQ(picture.width() % 2, 0u);
            EXPECT_EQ(picture.height() % 2, 0u);
            EXPECT_LE(picture.capture_time(), SteadyClock::instance().now());
        }
        for (size_t i = 1; i < pictures.size(); ++i)
            EXPECT_GT(pictures[i].capture_time(), pictures[i - 1].capture_time());
    }
}
