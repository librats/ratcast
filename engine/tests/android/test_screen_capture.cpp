// The screen on Android and what goes with it: where a MediaCodec buffer's
// planes are, by what its format says; the screen and what the applications
// play, which only an application's MediaProjection reaches — a process under
// `adb shell`, as ctest runs them, has no Java, so neither is there, and the
// applications' sound is not without the screen. The projection itself (the
// system's dialog, the screen turned, stopped) is checked in the app.

#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/screen_capture.h"

#include <gtest/gtest.h>

#include <media/NdkMediaFormat.h>

#include <cstring>
#include <memory>

#include "android/java.h"
#include "android/mediacodec.h"

using namespace ratcast;
using namespace ratcast::android;

namespace {

using Format = std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)>;

Format format_of(int32_t color, int32_t width, int32_t height, int32_t stride, int32_t rows) {
    Format format(AMediaFormat_new(), AMediaFormat_delete);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_COLOR_FORMAT, color);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_STRIDE, stride);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_SLICE_HEIGHT, rows);
    return format;
}

}  // namespace

TEST(MediaLayoutTest, ReadsTheTwoLayoutsAColourFormatNames) {
    const MediaLayout nv12 = layout_of(format_of(kColorFormatSemiPlanar, 64, 36, 128, 48).get());
    ASSERT_TRUE(nv12.known);
    EXPECT_TRUE(nv12.nv12());
    EXPECT_EQ(nv12.width, 64u);
    EXPECT_EQ(nv12.height, 36u);
    EXPECT_EQ(nv12.planes[0].row_step, 128);
    EXPECT_EQ(nv12.planes[1].offset, 128u * 48);  // after the luma's padded rows
    EXPECT_EQ(nv12.planes[2].offset, 128u * 48 + 1);
    EXPECT_TRUE(nv12.fits(128 * 48 * 3 / 2));
    EXPECT_FALSE(nv12.fits(128 * 48 + 128 * 17));  // the last chroma row cut short

    const MediaLayout i420 = layout_of(format_of(kColorFormatPlanar, 64, 36, 64, 36).get());
    ASSERT_TRUE(i420.known);
    EXPECT_FALSE(i420.nv12());
    EXPECT_EQ(i420.planes[1].offset, 64u * 36);
    EXPECT_EQ(i420.planes[2].offset, 64u * 36 + 32 * 18);
    EXPECT_EQ(i420.planes[2].row_step, 32);
    EXPECT_TRUE(i420.fits(64 * 36 * 3 / 2));

    EXPECT_FALSE(layout_of(format_of(kColorFormatFlexible, 64, 36, 64, 36).get()).known)
        << "flexible, with nothing to say where";
}

TEST(MediaLayoutTest, TakesWhatACodecDescribes) {
    // MediaImage2: type, planes, width, height, bit depth, allocated; then
    // each plane's offset, column step, row step and subsampling.
    int32_t image[4 + 2 + 4 * 5] = {1, 3, 64, 36, 8, 8};
    const int32_t planes[3][5] = {{0, 1, 80, 1, 1}, {4000, 2, 80, 2, 2}, {4001, 2, 80, 2, 2}};
    std::memcpy(image + 6, planes, sizeof planes);
    Format format = format_of(kColorFormatFlexible, 64, 36, 64, 36);
    AMediaFormat_setBuffer(format.get(), "image-data", image, sizeof image);
    AMediaFormat_setRect(format.get(), AMEDIAFORMAT_KEY_DISPLAY_CROP, 2, 2, 61, 33);

    const MediaLayout layout = layout_of(format.get());
    ASSERT_TRUE(layout.known);
    EXPECT_TRUE(layout.nv12());
    EXPECT_EQ(layout.planes[1].offset, 4000u);
    EXPECT_EQ(layout.planes[0].row_step, 80);
    EXPECT_EQ(layout.left, 2u);
    EXPECT_EQ(layout.top, 2u);
    EXPECT_EQ(layout.width, 60u);
    EXPECT_EQ(layout.height, 32u);
}

TEST(AndroidScreenTest, OffersNothingToChooseFromRefused) {
    ScreenCaptureOptions options;
    options.monitors = false;
    options.windows = false;
    Result<std::unique_ptr<ScreenCapture>> capture = open_screen_capture(options);
    ASSERT_FALSE(capture);
    EXPECT_EQ(capture.error(), Errc::InvalidArgument);
}

TEST(AndroidScreenTest, IsUnavailableOutsideAnApplication) {
    if (jni_context()) GTEST_SKIP() << "an application: the system's dialog would ask";
    Result<std::unique_ptr<ScreenCapture>> capture = open_screen_capture({});
    ASSERT_FALSE(capture);
    EXPECT_EQ(capture.error(), Errc::Unavailable);
}

TEST(AndroidScreenTest, WhatTheApplicationsPlayIsHeardOnlyBesideTheScreen) {
    Result<std::unique_ptr<AudioSource>> sound =
        open_audio_capture({.format = {48'000, 2}, .source = AudioCaptureSource::Applications});
    ASSERT_TRUE(sound);
    const Result<void> started = (*sound)->start([](AudioFrame) {}, nullptr);
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error(), Errc::Unavailable);
}
