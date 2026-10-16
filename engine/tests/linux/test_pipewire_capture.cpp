// pipewire::Capture against a stand-in screen on the user's PipeWire daemon:
// pictures arrive converted, cropped, at the size of the moment and no faster
// than asked, and the capture ends when the node goes. Skipped where there is
// no PipeWire (CI) — the portal in front of it has tests of its own.

#include "linux/pipewire_capture.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
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
    void add(VideoFrame picture) {
        std::lock_guard lock(mutex_);
        pictures_.push_back(std::move(picture));
        changed_.notify_all();
    }
    void end(Result<void> outcome) {
        std::lock_guard lock(mutex_);
        outcome_ = outcome;
        changed_.notify_all();
    }

    VideoSource::Sink sink() {
        return [this](VideoFrame picture) { add(std::move(picture)); };
    }
    VideoSource::EndHandler ended() {
        return [this](Result<void> outcome) { end(outcome); };
    }

    /// The first picture `match` accepts, waiting for it while `poke` runs
    /// every 10 ms (a node sends only when told to).
    std::optional<VideoFrame> wait_for(const std::function<bool(const VideoFrame&)>& match,
                                       const std::function<void()>& poke) {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        std::unique_lock lock(mutex_);
        for (size_t seen = 0;;) {
            for (; seen < pictures_.size(); ++seen) {
                if (match(pictures_[seen])) return pictures_[seen];
            }
            if (std::chrono::steady_clock::now() > deadline) return std::nullopt;
            lock.unlock();
            poke();
            lock.lock();
            changed_.wait_for(lock, 10ms);
        }
    }

    std::optional<Result<void>> wait_for_end() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, kPatience, [&] { return outcome_.has_value(); });
        return outcome_;
    }

    size_t count() {
        std::lock_guard lock(mutex_);
        return pictures_.size();
    }

    bool has_ended() {
        std::lock_guard lock(mutex_);
        return outcome_.has_value();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<VideoFrame> pictures_;
    std::optional<Result<void>> outcome_;
};

VideoFrame screen(uint32_t width, uint32_t height, Timestamp stamp) {
    return draw_pattern({PixelFormat::BGRA, width, height}, 0, stamp);
}

class PipeWireCaptureTest : public ::testing::Test {
protected:
    void SetUp() override {
        node_ = TestVideoNode::create(320, 240);
        if (!node_) GTEST_SKIP() << "no PipeWire daemon here";
    }

    std::unique_ptr<pipewire::Capture> capture(PixelFormat format, uint32_t max_rate = 0) {
        pipewire::CaptureConfig config;
        config.node = node_->node_id();
        config.format = format;
        config.max_frame_rate = max_rate;
        return std::make_unique<pipewire::Capture>(config);
    }

    /// Show `picture` until a picture of `width` x `height` comes out.
    std::optional<VideoFrame> capture_one(Received& received, const VideoFrame& picture,
                                          uint32_t width, uint32_t height,
                                          std::optional<TestVideoNode::Crop> crop = {}) {
        return received.wait_for(
            [&](const VideoFrame& p) { return p.width() == width && p.height() == height; },
            [&] { node_->show(picture, crop); });
    }

    std::unique_ptr<TestVideoNode> node_;
};

}  // namespace

TEST_F(PipeWireCaptureTest, PicturesComeConvertedIntoTheFormatAskedFor) {
    for (PixelFormat format : {PixelFormat::NV12, PixelFormat::I420, PixelFormat::BGRA}) {
        Received received;
        auto source = capture(format);
        ASSERT_TRUE(source->start(received.sink(), received.ended()));

        const Timestamp stamp{1234s};
        std::optional<VideoFrame> picture =
            capture_one(received, screen(320, 240, stamp), 320, 240);
        ASSERT_TRUE(picture) << "no picture in " << int(format);
        EXPECT_EQ(picture->format(), format);
        EXPECT_EQ(read_timestamp(*picture), stamp);
        EXPECT_EQ(picture->color_space(), ColorSpace{});
    }
}

TEST_F(PipeWireCaptureTest, CropsAsTheMetadataSays) {
    Received received;
    auto source = capture(PixelFormat::BGRA);
    ASSERT_TRUE(source->start(received.sink(), received.ended()));

    const VideoFrame full = screen(320, 240, Timestamp{1s});
    std::optional<VideoFrame> picture =
        capture_one(received, full, 200, 100, TestVideoNode::Crop{16, 8, 200, 100});
    ASSERT_TRUE(picture);
    for (uint32_t y = 0; y < 100; y += 33) {
        EXPECT_EQ(
            std::memcmp(picture->plane(0).data + y * picture->plane(0).stride,
                        full.plane(0).data + (y + 8) * full.plane(0).stride + 16 * 4, 200 * 4),
            0)
            << "row " << y;
    }
}

TEST_F(PipeWireCaptureTest, YuvPicturesHaveEvenSizes) {
    Received received;
    auto source = capture(PixelFormat::NV12);
    ASSERT_TRUE(source->start(received.sink(), received.ended()));
    node_->resize(321, 241);
    EXPECT_TRUE(capture_one(received, screen(321, 241, Timestamp{1s}), 320, 240));
}

TEST_F(PipeWireCaptureTest, FollowsTheNodeThroughAResize) {
    Received received;
    auto source = capture(PixelFormat::I420);
    ASSERT_TRUE(source->start(received.sink(), received.ended()));
    ASSERT_TRUE(capture_one(received, screen(320, 240, Timestamp{1s}), 320, 240));

    node_->resize(640, 360);
    std::optional<VideoFrame> bigger =
        capture_one(received, screen(640, 360, Timestamp{2s}), 640, 360);
    ASSERT_TRUE(bigger);
    EXPECT_EQ(read_timestamp(*bigger), Timestamp{2s});
}

TEST_F(PipeWireCaptureTest, EndsWhenTheNodeGoes) {
    Received received;
    auto source = capture(PixelFormat::NV12);
    ASSERT_TRUE(source->start(received.sink(), received.ended()));
    ASSERT_TRUE(capture_one(received, screen(320, 240, Timestamp{1s}), 320, 240));

    node_.reset();  // the window closed
    std::optional<Result<void>> outcome = received.wait_for_end();
    ASSERT_TRUE(outcome) << "the capture did not notice";
    EXPECT_TRUE(*outcome);
}

TEST_F(PipeWireCaptureTest, ANodeThatIsNotThereEndsTheCapture) {
    Received received;
    pipewire::CaptureConfig config;
    config.node = 0x7FFF'FFF0;  // no such node
    pipewire::Capture source(config);
    ASSERT_TRUE(source.start(received.sink(), received.ended()));
    std::optional<Result<void>> outcome = received.wait_for_end();
    ASSERT_TRUE(outcome);
    ASSERT_FALSE(*outcome);
    EXPECT_EQ(outcome->error(), Errc::Unavailable);
}

TEST_F(PipeWireCaptureTest, KeepsToTheMaximumRate) {
    Received received;
    auto source = capture(PixelFormat::NV12, 20);
    ASSERT_TRUE(source->start(received.sink(), received.ended()));
    ASSERT_TRUE(capture_one(received, screen(320, 240, Timestamp{1s}), 320, 240));

    const size_t before = received.count();
    const VideoFrame picture = screen(320, 240, Timestamp{2s});
    const auto until = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < until) {  // ~100 pictures a second
        node_->show(picture);
        std::this_thread::sleep_for(10ms);
    }
    const size_t delivered = received.count() - before;
    EXPECT_GE(delivered, 15u);
    EXPECT_LE(delivered, 22u);
}

TEST_F(PipeWireCaptureTest, StopsAndStartsAgain) {
    Received first;
    Received second;  // both outlive the source
    auto source = capture(PixelFormat::NV12);
    ASSERT_TRUE(source->start(first.sink(), first.ended()));
    EXPECT_FALSE(source->start(first.sink(), first.ended()));  // already running
    ASSERT_TRUE(capture_one(first, screen(320, 240, Timestamp{1s}), 320, 240));

    source->stop();
    const size_t delivered = first.count();
    for (int i = 0; i < 10; ++i) {
        node_->show(screen(320, 240, Timestamp{2s}));
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(first.count(), delivered);
    EXPECT_FALSE(first.has_ended());  // stopping is not ending

    ASSERT_TRUE(source->start(second.sink(), second.ended()));
    EXPECT_TRUE(capture_one(second, screen(320, 240, Timestamp{3s}), 320, 240));
}
