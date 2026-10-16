// The file source playing a clip the test writes itself: stamped pattern
// pictures through the engine's AV1 encoder into an IVF file, the simplest
// container FFmpeg reads. The stamps say which picture came out, in what
// order; the clock says when.

#include "ratcast/engine/file_source.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/video_codec.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;

/// The stamp on picture `i` of a test clip.
Timestamp stamp(uint64_t i) { return Timestamp{1s + std::chrono::milliseconds(i)}; }

template <typename T>
void put(std::ofstream& out, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) out.put(static_cast<char>(value >> (8 * i)));
}

/// A clip of `count` pattern pictures at `fps` as AV1 in IVF; false if this
/// build cannot encode AV1.
bool write_clip(const std::filesystem::path& path, int count, uint32_t fps) {
    VideoEncoderConfig config;
    config.codec = Codec::AV1;
    config.width = kWidth;
    config.height = kHeight;
    config.frame_rate_num = fps;
    config.bitrate = 1'000'000;
    Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config);
    if (!encoder) return false;

    std::ofstream out(path, std::ios::binary);
    out.write("DKIF", 4);
    put<uint16_t>(out, 0);   // version
    put<uint16_t>(out, 32);  // header size
    out.write("AV01", 4);
    put<uint16_t>(out, kWidth);
    put<uint16_t>(out, kHeight);
    put<uint32_t>(out, fps);  // time base: 1 / fps
    put<uint32_t>(out, 1);
    put<uint32_t>(out, static_cast<uint32_t>(count));
    put<uint32_t>(out, 0);

    const PatternConfig pattern{(*encoder)->info().input_format, kWidth, kHeight, fps, 1};
    for (int i = 0; i < count; ++i) {
        const auto write = [&](EncodedVideo&& frame) {
            put<uint32_t>(out, static_cast<uint32_t>(frame.payload.size()));
            put<uint64_t>(out, frame.info.number);
            out.write(reinterpret_cast<const char*>(frame.payload.data()),
                      static_cast<std::streamsize>(frame.payload.size()));
        };
        if (!(*encoder)->encode(draw_pattern(pattern, i, stamp(i)), i == 0, write)) return false;
    }
    return static_cast<bool>(out);
}

/// What the source handed out: which pictures (by stamp), when, and whether
/// and how it ended.
class Played {
public:
    VideoSource::Sink sink() {
        return [this](VideoFrame picture) {
            std::lock_guard lock(mutex_);
            pictures_.push_back(picture);
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

    /// Until `count` pictures are in, or a few seconds have passed.
    std::vector<VideoFrame> wait_for(size_t count) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, 5s, [&] { return pictures_.size() >= count; });
        return pictures_;
    }
    bool has_ended() {
        std::lock_guard lock(mutex_);
        return outcome_.has_value();
    }
    std::optional<Result<void>> wait_for_end() {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, 5s, [&] { return outcome_.has_value(); });
        return outcome_;
    }

    /// The index of the clip picture each one showed.
    std::vector<int> indexes() {
        std::lock_guard lock(mutex_);
        std::vector<int> out;
        for (const VideoFrame& p : pictures_) {
            const std::optional<Timestamp> t = read_timestamp(p);
            out.push_back(t ? static_cast<int>((*t - stamp(0)) / 1ms) : -1);
        }
        return out;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<VideoFrame> pictures_;
    std::optional<Result<void>> outcome_;
};

class FileSourceTest : public ::testing::Test {
protected:
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    /// A clip on disk; false if this build cannot make one.
    bool make_clip(int count, uint32_t fps) {
        path_ =
            std::filesystem::temp_directory_path() /
            ("ratcast-clip-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".ivf");
        return write_clip(path_, count, fps);
    }

    std::unique_ptr<FileSource> open(PixelFormat format, bool loop) {
        Result<std::unique_ptr<FileSource>> source =
            open_file_source({path_.string(), format, loop});
        if (!source) {
            ADD_FAILURE() << "cannot open the clip: " << to_string(source.error());
            return nullptr;
        }
        return std::move(*source);
    }

    std::filesystem::path path_;
};

}  // namespace

TEST_F(FileSourceTest, PlaysEveryPictureInOrderAndInTheFormatAskedFor) {
    if (!make_clip(10, 100)) GTEST_SKIP() << "no AV1 encoder here";
    for (PixelFormat format : {PixelFormat::I420, PixelFormat::NV12, PixelFormat::BGRA}) {
        Played played;  // declared first: it must outlive the source
        auto source = open(format, false);
        ASSERT_TRUE(source);
        EXPECT_EQ(source->info().width, kWidth);
        EXPECT_EQ(source->info().height, kHeight);
        EXPECT_EQ(source->info().frame_rate_num, 100u);
        EXPECT_EQ(source->info().frame_rate_den, 1u);

        ASSERT_TRUE(source->start(played.sink(), played.ended()));
        std::optional<Result<void>> outcome = played.wait_for_end();
        ASSERT_TRUE(outcome);
        EXPECT_TRUE(*outcome);  // the end of the file is a good end

        const std::vector<VideoFrame> pictures = played.wait_for(10);
        ASSERT_EQ(pictures.size(), 10u);
        EXPECT_EQ(played.indexes(), (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
        EXPECT_EQ(pictures[0].format(), format);
        EXPECT_EQ(pictures[0].width(), kWidth);
        source->stop();
    }
}

TEST_F(FileSourceTest, KeepsToTheFilesPace) {
    if (!make_clip(15, 30)) GTEST_SKIP() << "no AV1 encoder here";  // half a second
    Played played;
    auto source = open(PixelFormat::I420, false);
    ASSERT_TRUE(source);
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(source->start(played.sink(), played.ended()));
    ASSERT_TRUE(played.wait_for_end());
    const auto took = std::chrono::steady_clock::now() - start;
    // The last picture is due 14/30 s after the first.
    EXPECT_GE(took, 450ms);
    EXPECT_LT(took, 1500ms);

    const std::vector<VideoFrame> pictures = played.wait_for(15);
    ASSERT_EQ(pictures.size(), 15u);
    const Duration span = pictures.back().capture_time() - pictures.front().capture_time();
    EXPECT_NEAR(span.count(), 466'667, 60'000);  // capture times follow the file's timing
}

TEST_F(FileSourceTest, LoopsBackToTheStart) {
    if (!make_clip(5, 100)) GTEST_SKIP() << "no AV1 encoder here";
    Played played;
    auto source = open(PixelFormat::NV12, true);
    ASSERT_TRUE(source);
    ASSERT_TRUE(source->start(played.sink(), played.ended()));
    played.wait_for(12);
    source->stop();

    const std::vector<int> seen = played.indexes();
    ASSERT_GE(seen.size(), 12u);
    for (size_t i = 0; i < 12; ++i) EXPECT_EQ(seen[i], static_cast<int>(i % 5)) << i;
    EXPECT_FALSE(played.has_ended());  // a looping file never ends by itself
}

TEST_F(FileSourceTest, StartsFromTheBeginningEachTime) {
    if (!make_clip(30, 100)) GTEST_SKIP() << "no AV1 encoder here";
    Played first;
    Played second;
    auto source = open(PixelFormat::I420, false);
    ASSERT_TRUE(source);

    ASSERT_TRUE(source->start(first.sink(), first.ended()));
    first.wait_for(3);
    source->stop();

    ASSERT_TRUE(source->start(second.sink(), second.ended()));
    second.wait_for(3);
    source->stop();
    ASSERT_FALSE(second.indexes().empty());
    EXPECT_EQ(second.indexes()[0], 0);
}

TEST_F(FileSourceTest, RefusesWhatItCannotPlay) {
    if (!open_video_decoder({Codec::H264, {}})) GTEST_SKIP() << "built without FFmpeg";
    Result<std::unique_ptr<FileSource>> missing =
        open_file_source({"/nonexistent/ratcast/clip.mkv", PixelFormat::NV12, true});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error(), Errc::Unavailable);

    path_ = std::filesystem::temp_directory_path() / "ratcast-not-a-video.bin";
    std::ofstream(path_) << std::string(4096, 'z');
    Result<std::unique_ptr<FileSource>> garbage =
        open_file_source({path_.string(), PixelFormat::NV12, true});
    ASSERT_FALSE(garbage);
    EXPECT_EQ(garbage.error(), Errc::Unsupported);
}
