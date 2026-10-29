// Cameras through Camera2: a picture of any plane steps turned upright into
// NV12 and I420; the cameras listed with nodes that hold, the front ones
// first; captured into 4:2:0 at even sizes no larger than asked, at the time
// they were taken; a camera that is not there ending its capture. The
// capture is an application's, with the Context it is given: a process under
// `adb shell`, as ctest runs them, may be given no pictures, its camera then
// hanging as it closes (MIUI), so it is tested there only where asked
// (RATCAST_TEST_CAMERA set), and skips where the camera is not ours to use.

#include "ratcast/engine/camera_capture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include "android/camera_picture.h"
#include "android/java.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

/// A 4 x 2 picture whose samples say where they are: Y = 10·y + x, U = 100
/// + 10·y + x, V = 200 + 10·y + x (chroma 2 x 1). Chroma interleaved (step
/// 2), as most cameras give it, or in planes of its own.
struct Source {
    explicit Source(bool interleaved) {
        for (uint8_t y = 0; y < 2; ++y)
            for (uint8_t x = 0; x < 4; ++x) luma[y * 8 + x] = uint8_t(10 * y + x);
        for (uint8_t x = 0; x < 2; ++x) {
            if (interleaved) {
                chroma[2 * x] = uint8_t(100 + x);
                chroma[2 * x + 1] = uint8_t(200 + x);
            } else {
                chroma[x] = uint8_t(100 + x);
                chroma[8 + x] = uint8_t(200 + x);
            }
        }
        planes.width = 4;
        planes.height = 2;
        planes.y = {luma, 8, 1};  // rows padded to 8
        planes.u = interleaved ? android::Yuv420Planes::Plane{chroma, 8, 2}
                               : android::Yuv420Planes::Plane{chroma, 8, 1};
        planes.v = interleaved ? android::Yuv420Planes::Plane{chroma + 1, 8, 2}
                               : android::Yuv420Planes::Plane{chroma + 8, 8, 1};
    }
    uint8_t luma[16] = {};
    uint8_t chroma[16] = {};
    android::Yuv420Planes planes;
};

/// The samples of `picture`'s plane `i`, row by row; NV12's chroma as U, V.
std::vector<std::vector<int>> rows_of(const VideoFrame& picture, size_t i) {
    const uint32_t width = i == 0 ? picture.width() : picture.width() / 2;
    const uint32_t height = i == 0 ? picture.height() : picture.height() / 2;
    const size_t per_sample = picture.format() == PixelFormat::NV12 && i == 1 ? 2 : 1;
    std::vector<std::vector<int>> rows;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = picture.plane(i).data + y * picture.plane(i).stride;
        rows.emplace_back(row, row + width * per_sample);
    }
    return rows;
}

VideoFrame turn(const android::Yuv420Planes& planes, int degrees, PixelFormat format) {
    MutableVideoFrame picture = MutableVideoFrame::allocate(
        format, android::turned_width(planes.width, planes.height, degrees),
        android::turned_height(planes.width, planes.height, degrees));
    android::copy_turned(planes, degrees, picture);
    return std::move(picture).freeze(Timestamp{});
}

using Rows = std::vector<std::vector<int>>;

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

TEST(CameraPictureTest, IsTurnedClockwiseIntoNv12) {
    for (const bool interleaved : {true, false}) {
        const Source source(interleaved);
        SCOPED_TRACE(interleaved ? "interleaved" : "planar");

        const VideoFrame upright = turn(source.planes, 0, PixelFormat::NV12);
        EXPECT_EQ(rows_of(upright, 0), (Rows{{0, 1, 2, 3}, {10, 11, 12, 13}}));
        EXPECT_EQ(rows_of(upright, 1), (Rows{{100, 200, 101, 201}}));

        const VideoFrame quarter = turn(source.planes, 90, PixelFormat::NV12);
        ASSERT_EQ(quarter.width(), 2u);
        ASSERT_EQ(quarter.height(), 4u);
        EXPECT_EQ(rows_of(quarter, 0), (Rows{{10, 0}, {11, 1}, {12, 2}, {13, 3}}));
        EXPECT_EQ(rows_of(quarter, 1), (Rows{{100, 200}, {101, 201}}));

        const VideoFrame half = turn(source.planes, 180, PixelFormat::NV12);
        EXPECT_EQ(rows_of(half, 0), (Rows{{13, 12, 11, 10}, {3, 2, 1, 0}}));
        EXPECT_EQ(rows_of(half, 1), (Rows{{101, 201, 100, 200}}));

        const VideoFrame three = turn(source.planes, 270, PixelFormat::NV12);
        EXPECT_EQ(rows_of(three, 0), (Rows{{3, 13}, {2, 12}, {1, 11}, {0, 10}}));
        EXPECT_EQ(rows_of(three, 1), (Rows{{101, 201}, {100, 200}}));
    }
}

TEST(CameraPictureTest, IsTurnedIntoI420) {
    const Source source(true);
    const VideoFrame quarter = turn(source.planes, 90, PixelFormat::I420);
    EXPECT_EQ(rows_of(quarter, 0), (Rows{{10, 0}, {11, 1}, {12, 2}, {13, 3}}));
    EXPECT_EQ(rows_of(quarter, 1), (Rows{{100}, {101}}));
    EXPECT_EQ(rows_of(quarter, 2), (Rows{{200}, {201}}));
}

TEST(AndroidCameraTest, AreListedWithNodesThatHoldFrontFirst) {
    const Result<std::vector<CameraInfo>> cameras = list_cameras();
    ASSERT_TRUE(cameras);
    const Result<std::vector<CameraInfo>> again = list_cameras();
    ASSERT_TRUE(again);
    ASSERT_EQ(again->size(), cameras->size());
    std::set<uint32_t> nodes;
    bool back_seen = false;
    for (size_t i = 0; i < cameras->size(); ++i) {
        const CameraInfo& camera = (*cameras)[i];
        EXPECT_NE(camera.node, 0u);
        EXPECT_FALSE(camera.id.empty());
        EXPECT_FALSE(camera.name.empty());
        EXPECT_TRUE(nodes.insert(camera.node).second) << "two cameras of one node";
        EXPECT_EQ((*again)[i].node, camera.node);
        const bool front = camera.name.starts_with("Front");
        EXPECT_FALSE(front && back_seen) << "a front camera after a back one";
        back_seen = back_seen || camera.name.starts_with("Back");
    }
}

TEST(AndroidCameraTest, OneThatIsNotThereEnds) {
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

TEST(AndroidCameraTest, CapturesInto420AtEvenSizes) {
    if (!android::jni_context() && !std::getenv("RATCAST_TEST_CAMERA"))
        GTEST_SKIP() << "not an application: RATCAST_TEST_CAMERA=1 to try";
    const Result<std::vector<CameraInfo>> cameras = list_cameras();
    ASSERT_TRUE(cameras);
    if (cameras->empty()) GTEST_SKIP() << "no camera here";
    for (const CameraInfo& info : *cameras) {
        for (const PixelFormat format : {PixelFormat::NV12, PixelFormat::I420}) {
            SCOPED_TRACE(info.name + (format == PixelFormat::NV12 ? ", NV12" : ", I420"));
            Result<std::unique_ptr<VideoSource>> camera = open_camera(
                {.node = info.node, .format = format, .max_width = 640, .max_height = 480});
            ASSERT_TRUE(camera);
            Seen seen;
            if (!(*camera)->start(seen.sink(), seen.ended()))
                GTEST_SKIP() << "the camera is not ours to use";
            const std::vector<VideoFrame> pictures = seen.wait_for(5);
            (*camera)->stop();
            if (pictures.empty()) GTEST_SKIP() << "opened, but given no pictures here";
            ASSERT_GE(pictures.size(), 5u);
            for (const VideoFrame& picture : pictures) {
                EXPECT_EQ(picture.format(), format);
                EXPECT_EQ(picture.width() % 2, 0u);
                EXPECT_EQ(picture.height() % 2, 0u);
                // As the sensor sees it, or turned.
                EXPECT_LE(std::min(picture.width(), picture.height()), 480u);
                EXPECT_LE(std::max(picture.width(), picture.height()), 640u);
                EXPECT_LE(picture.capture_time(), SteadyClock::instance().now());
            }
            for (size_t i = 1; i < pictures.size(); ++i)
                EXPECT_GT(pictures[i].capture_time(), pictures[i - 1].capture_time());
        }
    }
}
