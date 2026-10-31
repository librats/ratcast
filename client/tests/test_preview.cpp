// A sender's preview of what it shares: few pictures, small, and none at all
// while nobody looks.

#include "ratcast/client/preview.h"

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "ratcast/engine/pattern_source.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

/// What a preview showed.
class Shown {
public:
    VideoSource::Sink sink() {
        return [this](VideoFrame picture) {
            std::lock_guard lock(mutex_);
            pictures_.push_back(std::move(picture));
        };
    }
    size_t count() {
        std::lock_guard lock(mutex_);
        return pictures_.size();
    }
    VideoFrame last() {
        std::lock_guard lock(mutex_);
        return pictures_.empty() ? VideoFrame{} : pictures_.back();
    }
    /// Until `n` have been shown, or a while has passed.
    bool reach(size_t n) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (count() < n) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::vector<VideoFrame> pictures_;
};

VideoFrame picture(Timestamp at, uint32_t width = 64, uint32_t height = 64) {
    return draw_pattern({PixelFormat::NV12, width, height}, 0, at);
}

}  // namespace

TEST(Preview, ShowsAtMostItsFrameRate) {
    Preview preview({.frame_rate = 15});
    Shown shown;
    preview.set_sink(shown.sink());
    // 60 a second for two seconds: every fourth shown, the rest passed over.
    constexpr Duration kApart{16'667};
    for (int i = 0; i < 120; ++i) {
        preview.take(picture(Timestamp{10s} + i * kApart));
        if (i % 4 == 0) {
            ASSERT_TRUE(shown.reach(size_t(i / 4 + 1))) << i;
        }
    }
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(shown.count(), 30u);
}

TEST(Preview, ShowsAPictureSmall) {
    Preview preview({.max_width = 640, .max_height = 360});
    Shown shown;
    preview.set_sink(shown.sink());
    preview.take(picture(Timestamp{1s}, 2560, 1440));
    ASSERT_TRUE(shown.reach(1));
    EXPECT_EQ(shown.last().width(), 640u);
    EXPECT_EQ(shown.last().height(), 360u);
    EXPECT_EQ(shown.last().capture_time(), Timestamp{1s});

    // One that fits already, as it is.
    const VideoFrame small = picture(Timestamp{2s}, 320, 240);
    preview.take(small);
    ASSERT_TRUE(shown.reach(2));
    EXPECT_EQ(shown.last().plane(0).data, small.plane(0).data);
}

TEST(Preview, TakesNothingWhileNobodyLooks) {
    Preview preview;
    Shown shown;
    preview.take(picture(Timestamp{1s}));
    preview.set_sink(shown.sink());
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(shown.count(), 0u) << "none kept from before";

    preview.take(picture(Timestamp{2s}));
    ASSERT_TRUE(shown.reach(1));
    preview.set_sink({});
    preview.take(picture(Timestamp{3s}));
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(shown.count(), 1u);
}

TEST(Preview, StartsAgainWithASourceOnAnotherClock) {
    Preview preview;
    Shown shown;
    preview.set_sink(shown.sink());
    preview.take(picture(Timestamp{100s}));
    ASSERT_TRUE(shown.reach(1));
    preview.take(picture(Timestamp{5s}));
    EXPECT_TRUE(shown.reach(2)) << "long before the last: shown, not waited for";
}
