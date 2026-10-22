// Cameras through Media Foundation: the mode taken, as Linux takes one; each
// format a camera sends — YUY2, NV12, I420, RGB32 bottom row first — read out
// of a sample's buffer into 4:2:0 exactly; nodes that hold from one listing to
// the next. And the cameras there are: listed, and captured into 4:2:0 at even
// sizes; a camera that is not there ending its capture. Those last skip where
// there is no camera.

#include "windows/media_foundation_camera.h"

#include <gtest/gtest.h>

#include <mfapi.h>
#include <wrl/client.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include "ratcast/engine/camera_capture.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/video_convert.h"

using namespace ratcast;
using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;

namespace {

constexpr auto kPatience = 5s;
constexpr uint32_t kWidth = 96;
constexpr uint32_t kHeight = 64;

mf::CameraMode mode(uint32_t width, uint32_t height, double rate) {
    return {.width = width, .height = height, .frame_rate = rate};
}

/// A buffer holding `bytes`, as a camera's sample would.
ComPtr<IMFMediaBuffer> buffer_of(const std::vector<uint8_t>& bytes) {
    ComPtr<IMFMediaBuffer> buffer;
    EXPECT_TRUE(SUCCEEDED(MFCreateMemoryBuffer(DWORD(bytes.size()), &buffer)));
    BYTE* data = nullptr;
    buffer->Lock(&data, nullptr, nullptr);
    std::memcpy(data, bytes.data(), bytes.size());
    buffer->Unlock();
    buffer->SetCurrentLength(DWORD(bytes.size()));
    return buffer;
}

/// `picture`'s planes one after another, rows `width` bytes (half that for
/// I420's chroma) apart.
std::vector<uint8_t> packed(const VideoFrame& picture) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < picture.plane_count(); ++i) {
        const size_t row = plane_row_bytes(picture.format(), i, picture.width());
        for (size_t y = 0; y < plane_rows(picture.format(), i, picture.height()); ++y) {
            const uint8_t* from = picture.plane(i).data + picture.plane(i).stride * y;
            bytes.insert(bytes.end(), from, from + row);
        }
    }
    return bytes;
}

/// The I420 `picture` as a camera sends it in YUY2: each chroma row twice.
std::vector<uint8_t> yuy2_of(const VideoFrame& picture) {
    std::vector<uint8_t> bytes;
    for (uint32_t y = 0; y < picture.height(); ++y) {
        const uint8_t* luma = picture.plane(0).data + picture.plane(0).stride * y;
        const uint8_t* u = picture.plane(1).data + picture.plane(1).stride * (y / 2);
        const uint8_t* v = picture.plane(2).data + picture.plane(2).stride * (y / 2);
        for (uint32_t x = 0; x < picture.width(); x += 2) {
            bytes.insert(bytes.end(), {luma[x], u[x / 2], luma[x + 1], v[x / 2]});
        }
    }
    return bytes;
}

void expect_same(const VideoFrame& a, const VideoFrame& b) {
    ASSERT_EQ(a.format(), b.format());
    ASSERT_EQ(a.width(), b.width());
    ASSERT_EQ(a.height(), b.height());
    EXPECT_EQ(packed(a), packed(b));
}

class MediaFoundationTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))); }
    void TearDown() override { MFShutdown(); }

    const VideoFrame reference_ = draw_pattern(
        {.format = PixelFormat::I420, .width = kWidth, .height = kHeight}, 3, Timestamp{1s});
};

}  // namespace

TEST(CameraModeTest, TakesTheFirstNoLargerAndSmooth) {
    const std::vector<mf::CameraMode> modes = {mode(1920, 1080, 30), mode(1280, 720, 5),
                                               mode(1280, 720, 30), mode(640, 480, 30)};
    EXPECT_EQ(mf::choose_mode(modes, 1280, 720), 2u);
}

TEST(CameraModeTest, ElseTheFirstNoLarger) {
    const std::vector<mf::CameraMode> modes = {mode(1920, 1080, 30), mode(1280, 720, 5),
                                               mode(640, 480, 10)};
    EXPECT_EQ(mf::choose_mode(modes, 1280, 720), 1u);
}

TEST(CameraModeTest, ElseItsFirst) {
    const std::vector<mf::CameraMode> modes = {mode(3840, 2160, 30), mode(1920, 1080, 60)};
    EXPECT_EQ(mf::choose_mode(modes, 1280, 720), 0u);
    EXPECT_EQ(mf::choose_mode({}, 1280, 720), std::nullopt);
}

TEST(CameraModeTest, NodesHoldAndAreNeverZero) {
    const wchar_t link[] = L"\\\\?\\usb#vid_046d&pid_0825&mi_00#7&1a2b&0&0000#{e5323777}\\global";
    EXPECT_EQ(mf::node_of(link), mf::node_of(link));
    EXPECT_EQ(mf::node_of(link),
              mf::node_of(L"\\\\?\\USB#VID_046D&PID_0825&MI_00#7&1A2B&0&0000#{E5323777}\\GLOBAL"));
    EXPECT_NE(mf::node_of(link), mf::node_of(L"\\\\?\\usb#vid_046d&pid_0826"));
    EXPECT_NE(mf::node_of(L""), 0u);
}

TEST_F(MediaFoundationTest, ReadsYuy2IntoFourTwoZeroExactly) {
    mf::CameraMode yuy2 = mode(kWidth, kHeight, 30);
    yuy2.format = PixelFormat::YUY2;
    ComPtr<IMFMediaBuffer> buffer = buffer_of(yuy2_of(reference_));
    const std::optional<VideoFrame> picture =
        mf::read_picture(*buffer.Get(), yuy2, PixelFormat::I420, Timestamp{2s});
    ASSERT_TRUE(picture);
    expect_same(*picture, reference_);
    EXPECT_EQ(picture->capture_time(), Timestamp{2s});
    EXPECT_EQ(picture->color_space(), (ColorSpace{ColorMatrix::BT601, ColorRange::Limited}));
}

TEST_F(MediaFoundationTest, ReadsNv12AndI420InOneBlock) {
    for (const PixelFormat sent : {PixelFormat::NV12, PixelFormat::I420}) {
        Result<VideoFrame> as_sent = convert_frame(reference_, sent);
        ASSERT_TRUE(as_sent);
        mf::CameraMode camera = mode(kWidth, kHeight, 30);
        camera.format = sent;
        ComPtr<IMFMediaBuffer> buffer = buffer_of(packed(*as_sent));
        for (const PixelFormat wanted : {PixelFormat::NV12, PixelFormat::I420}) {
            const std::optional<VideoFrame> picture =
                mf::read_picture(*buffer.Get(), camera, wanted, Timestamp{2s});
            ASSERT_TRUE(picture);
            Result<VideoFrame> expected = convert_frame(reference_, wanted);
            expect_same(*picture, *expected);
        }
    }
}

TEST_F(MediaFoundationTest, ReadsRgbStoredBottomRowFirstTheRightWayUp) {
    // A picture in BGRA, then stored as RGB32 cameras store it: last row first.
    MutableVideoFrame drawn = MutableVideoFrame::allocate(PixelFormat::BGRA, kWidth, kHeight);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            uint8_t* pixel = drawn.plane(0).data + drawn.plane(0).stride * y + x * 4;
            pixel[0] = uint8_t(x * 4);
            pixel[1] = uint8_t(y * 5);
            pixel[2] = uint8_t(200 - x);
            pixel[3] = 255;
        }
    }
    const VideoFrame upright = std::move(drawn).freeze(Timestamp{1s});
    std::vector<uint8_t> bytes;
    for (uint32_t y = kHeight; y-- > 0;) {
        const uint8_t* row = upright.plane(0).data + upright.plane(0).stride * y;
        bytes.insert(bytes.end(), row, row + kWidth * 4);
    }
    mf::CameraMode rgb = mode(kWidth, kHeight, 30);
    rgb.format = PixelFormat::BGRA;
    rgb.stride = -int32_t(kWidth * 4);
    ComPtr<IMFMediaBuffer> buffer = buffer_of(bytes);
    const std::optional<VideoFrame> picture =
        mf::read_picture(*buffer.Get(), rgb, PixelFormat::NV12, Timestamp{2s});
    ASSERT_TRUE(picture);
    expect_same(*picture, *convert_frame(upright, PixelFormat::NV12));
}

TEST_F(MediaFoundationTest, DropsABufferTooSmallForItsPicture) {
    mf::CameraMode nv12 = mode(kWidth, kHeight, 30);
    nv12.format = PixelFormat::NV12;
    ComPtr<IMFMediaBuffer> buffer = buffer_of(std::vector<uint8_t>(kWidth * kHeight, 16));
    EXPECT_FALSE(mf::read_picture(*buffer.Get(), nv12, PixelFormat::NV12, Timestamp{2s}));
}

TEST(CameraTest, ACameraThatIsNotThereEndsItsCapture) {
    Result<std::unique_ptr<VideoSource>> camera = open_camera({.node = 12345});
    ASSERT_TRUE(camera);
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Result<void>> outcome;
    ASSERT_TRUE((*camera)->start([](VideoFrame) {},
                                 [&](Result<void> ended) {
                                     std::lock_guard lock(mutex);
                                     outcome = ended;
                                     changed.notify_all();
                                 }));
    std::unique_lock lock(mutex);
    changed.wait_for(lock, kPatience, [&] { return outcome.has_value(); });
    ASSERT_TRUE(outcome);
    ASSERT_FALSE(*outcome);
    EXPECT_EQ(outcome->error(), Errc::Unavailable);
}

TEST(CameraTest, TheCamerasHereAreCapturedInFourTwoZero) {
    Result<std::vector<CameraInfo>> cameras = list_cameras();
    ASSERT_TRUE(cameras);
    if (cameras->empty()) {
        Result<std::unique_ptr<VideoSource>> none = open_camera();
        ASSERT_FALSE(none);
        EXPECT_EQ(none.error(), Errc::Unavailable);
        GTEST_SKIP() << "no camera here";
    }
    for (const CameraInfo& info : *cameras) EXPECT_FALSE(info.name.empty());

    Result<std::unique_ptr<VideoSource>> camera =
        open_camera({.node = cameras->front().node, .max_width = 640, .max_height = 480});
    ASSERT_TRUE(camera);
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<VideoFrame> pictures;
    if (!(*camera)->start(
            [&](VideoFrame picture) {
                std::lock_guard lock(mutex);
                pictures.push_back(std::move(picture));
                changed.notify_all();
            },
            {})) {
        GTEST_SKIP() << "the camera is in use, or not allowed";
    }
    std::unique_lock lock(mutex);
    changed.wait_for(lock, kPatience, [&] { return pictures.size() >= 3; });
    ASSERT_GE(pictures.size(), 3u);
    for (const VideoFrame& picture : pictures) {
        EXPECT_EQ(picture.format(), PixelFormat::NV12);
        EXPECT_EQ(picture.width() % 2, 0u);
        EXPECT_EQ(picture.height() % 2, 0u);
    }
    lock.unlock();
    (*camera)->stop();
}
