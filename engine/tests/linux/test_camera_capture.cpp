// Cameras against stand-ins on the user's PipeWire daemon: listed by their
// role (a screen is not one) with an id that lasts, chosen by it, captured in what cameras send —
// YUY2, NV12 and I420, planes in one block or a block each — into 4:2:0 exactly, a camera with only
// larger modes than asked taken as it is, and the capture ending when the camera goes. Skipped
// where there is no PipeWire (CI).

#include "ratcast/engine/camera_capture.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ratcast/engine/pattern_source.h"
#include "test_video_node.h"

using namespace ratcast;
using namespace std::chrono_literals;
using test::TestVideoNode;

namespace {

constexpr auto kPatience = 5s;

/// What a capture handed out, for the test thread to wait on.
class Received {
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

    /// The first picture, showing `picture` on `node` every 10 ms until then.
    std::optional<VideoFrame> first(TestVideoNode& node, const VideoFrame& picture) {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        std::unique_lock lock(mutex_);
        while (pictures_.empty() && std::chrono::steady_clock::now() < deadline) {
            lock.unlock();
            node.show(picture);
            lock.lock();
            changed_.wait_for(lock, 10ms);
        }
        if (pictures_.empty()) return std::nullopt;
        return pictures_.front();
    }

    std::optional<Result<void>> wait_for_end() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, kPatience, [&] { return outcome_.has_value(); });
        return outcome_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<VideoFrame> pictures_;
    std::optional<Result<void>> outcome_;
};

/// A name of this process's own, so that tests side by side tell theirs apart.
std::string camera_name() { return "ratcast test camera " + std::to_string(getpid()); }

/// The stamped pattern in `format`; YUY2's chroma the same on each two rows,
/// so that it comes back into 4:2:0 as the pattern was.
VideoFrame picture(PixelFormat format, uint32_t width, uint32_t height, Timestamp stamp) {
    if (format != PixelFormat::YUY2) return draw_pattern({format, width, height}, 0, stamp);
    const VideoFrame i420 = draw_pattern({PixelFormat::I420, width, height}, 0, stamp);
    MutableVideoFrame packed = MutableVideoFrame::allocate(PixelFormat::YUY2, width, height);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = packed.plane(0).data + y * packed.plane(0).stride;
        const auto plane = [&](size_t i, uint32_t x, uint32_t line) {
            return i420.plane(i).data[line * i420.plane(i).stride + x];
        };
        for (uint32_t x = 0; x < width; ++x) row[2 * x] = plane(0, x, y);
        for (uint32_t x = 0; x < width / 2; ++x) {
            row[4 * x + 1] = plane(1, x, y / 2);
            row[4 * x + 3] = plane(2, x, y / 2);
        }
    }
    return std::move(packed).freeze(stamp);
}

bool same_planes(const VideoFrame& a, const VideoFrame& b) {
    if (a.format() != b.format() || a.width() != b.width() || a.height() != b.height())
        return false;
    for (size_t i = 0; i < a.plane_count(); ++i) {
        for (size_t y = 0; y < plane_rows(a.format(), i, a.height()); ++y) {
            if (std::memcmp(a.plane(i).data + y * a.plane(i).stride,
                            b.plane(i).data + y * b.plane(i).stride,
                            plane_row_bytes(a.format(), i, a.width())) != 0) {
                return false;
            }
        }
    }
    return true;
}

std::unique_ptr<TestVideoNode> camera(PixelFormat format, uint32_t width = 320,
                                      uint32_t height = 240, bool block_per_plane = false,
                                      std::string name = camera_name()) {
    return TestVideoNode::create(width, height,
                                 {.format = format,
                                  .camera = true,
                                  .name = std::move(name),
                                  .block_per_plane = block_per_plane});
}

}  // namespace

TEST(CameraCapture, ListsCamerasButNotScreens) {
    auto node = camera(PixelFormat::YUY2);
    if (!node) GTEST_SKIP() << "no PipeWire daemon here";
    auto screen = TestVideoNode::create(320, 240);
    ASSERT_TRUE(screen);

    Result<std::vector<CameraInfo>> cameras = list_cameras();
    ASSERT_TRUE(cameras);
    bool listed = false;
    for (const CameraInfo& info : *cameras) {
        EXPECT_NE(info.node, screen->node_id()) << "a screen is no camera";
        if (info.node == node->node_id()) {
            listed = true;
            EXPECT_EQ(info.name, camera_name());
            EXPECT_EQ(info.id, camera_name()) << "its node's name, which lasts";
        }
    }
    EXPECT_TRUE(listed);
}

TEST(CameraCapture, ACameraIsChosenByItsIdWhateverItsNode) {
    auto first = camera(PixelFormat::NV12);
    if (!first) GTEST_SKIP() << "no PipeWire daemon here";
    const std::string id = camera_name() + " chosen";
    auto chosen = camera(PixelFormat::NV12, 320, 240, false, id);
    ASSERT_TRUE(chosen);
    Result<std::unique_ptr<VideoSource>> source = open_camera({.id = id});
    ASSERT_TRUE(source);
    Received received;
    ASSERT_TRUE((*source)->start(received.sink(), received.ended()));
    EXPECT_TRUE(received.first(*chosen, picture(PixelFormat::NV12, 320, 240, Timestamp{1s})))
        << "another camera taken";
}

TEST(CameraCapture, TakesWhatCamerasSendInto420Exactly) {
    struct Case {
        PixelFormat format;
        bool block_per_plane;
    };
    for (const Case& c : {Case{PixelFormat::YUY2, false}, Case{PixelFormat::NV12, false},
                          Case{PixelFormat::NV12, true}, Case{PixelFormat::I420, false},
                          Case{PixelFormat::I420, true}}) {
        SCOPED_TRACE(::testing::Message() << "format " << int(c.format) << ", a block "
                                          << (c.block_per_plane ? "a plane" : "for all"));
        auto node = camera(c.format, 320, 240, c.block_per_plane);
        if (!node) GTEST_SKIP() << "no PipeWire daemon here";
        Result<std::unique_ptr<VideoSource>> source =
            open_camera({.node = node->node_id(), .format = PixelFormat::NV12});
        ASSERT_TRUE(source);
        Received received;
        ASSERT_TRUE((*source)->start(received.sink(), received.ended()));

        const Timestamp stamp{77s};
        const std::optional<VideoFrame> captured =
            received.first(*node, picture(c.format, 320, 240, stamp));
        ASSERT_TRUE(captured) << "nothing captured";
        EXPECT_TRUE(same_planes(*captured, picture(PixelFormat::NV12, 320, 240, stamp)));
        EXPECT_EQ(captured->color_space(), (ColorSpace{ColorMatrix::BT601, ColorRange::Limited}))
            << "a camera's, unless it says";
    }
}

TEST(CameraCapture, ACameraWithOnlyLargerModesIsTakenAsItIs) {
    auto node = camera(PixelFormat::NV12, 640, 480);
    if (!node) GTEST_SKIP() << "no PipeWire daemon here";
    Result<std::unique_ptr<VideoSource>> source =
        open_camera({.node = node->node_id(), .max_width = 320, .max_height = 240});
    ASSERT_TRUE(source);
    Received received;
    ASSERT_TRUE((*source)->start(received.sink(), received.ended()));
    const std::optional<VideoFrame> captured =
        received.first(*node, picture(PixelFormat::NV12, 640, 480, Timestamp{1s}));
    ASSERT_TRUE(captured);
    EXPECT_EQ(captured->width(), 640u);
}

TEST(CameraCapture, EndsWhenTheCameraGoes) {
    auto node = camera(PixelFormat::YUY2);
    if (!node) GTEST_SKIP() << "no PipeWire daemon here";
    Result<std::unique_ptr<VideoSource>> source = open_camera({.node = node->node_id()});
    ASSERT_TRUE(source);
    Received received;
    ASSERT_TRUE((*source)->start(received.sink(), received.ended()));
    ASSERT_TRUE(received.first(*node, picture(PixelFormat::YUY2, 320, 240, Timestamp{1s})));

    node.reset();  // unplugged
    const std::optional<Result<void>> outcome = received.wait_for_end();
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(*outcome);
}

TEST(CameraCapture, RefusesWhatItCannotGive) {
    EXPECT_EQ(open_camera({.format = PixelFormat::BGRA}).error(), Errc::InvalidArgument);
    EXPECT_EQ(open_camera({.format = PixelFormat::YUY2}).error(), Errc::InvalidArgument);
    EXPECT_EQ(open_camera({.max_width = 0}).error(), Errc::InvalidArgument);
    EXPECT_EQ(open_camera({.max_frame_rate = 0}).error(), Errc::InvalidArgument);
}
