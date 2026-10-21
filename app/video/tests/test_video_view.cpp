// VideoView drawn for real and read back from windows never shown, on
// whatever the display's Qt Quick draws with (a GPU, or Mesa's software
// rasteriser; CI runs them under Xvfb). Pictures of four colours, in each
// layout and colour space, must come out in those colours, fitted and
// centred, with the rest of the window left as it was. Without a display the
// drawing tests skip: Qt's offscreen platform draws no shaders.

#include "video_view.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <cstdlib>
#include <memory>
#include <thread>

#include "ratcast/engine/video_convert.h"
#include "video_node.h"

using namespace ratcast;
using namespace ratcast::app;

namespace {

constexpr int kTolerance = 6;  ///< per channel, of 255: 8-bit samples, filtering

/// Top left, top right, bottom left, bottom right.
constexpr QRgb kQuadrants[4] = {qRgb(220, 30, 30), qRgb(30, 200, 60), qRgb(40, 60, 220),
                                qRgb(240, 240, 240)};

/// A picture of the four colours, one to a quadrant, in `format` and `space`.
VideoFrame quadrants(uint32_t width, uint32_t height, PixelFormat format, ColorSpace space) {
    MutableVideoFrame bgra = MutableVideoFrame::allocate(PixelFormat::BGRA, width, height);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = bgra.plane(0).data + y * bgra.plane(0).stride;
        for (uint32_t x = 0; x < width; ++x) {
            const QRgb colour = kQuadrants[(y < height / 2 ? 0 : 2) + (x < width / 2 ? 0 : 1)];
            row[4 * x] = uint8_t(qBlue(colour));
            row[4 * x + 1] = uint8_t(qGreen(colour));
            row[4 * x + 2] = uint8_t(qRed(colour));
            row[4 * x + 3] = 0xFF;
        }
    }
    return convert_frame(std::move(bgra).freeze(Timestamp{}), format, space).value();
}

::testing::AssertionResult near(QRgb actual, QRgb expected) {
    const bool close = std::abs(qRed(actual) - qRed(expected)) <= kTolerance &&
                       std::abs(qGreen(actual) - qGreen(expected)) <= kTolerance &&
                       std::abs(qBlue(actual) - qBlue(expected)) <= kTolerance;
    if (close) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "drawn " << qRed(actual) << "," << qGreen(actual) << "," << qBlue(actual)
           << ", wanted " << qRed(expected) << "," << qGreen(expected) << "," << qBlue(expected);
}

class VideoViewTest : public ::testing::Test {
protected:
    void SetUp() override {
        window_ = std::make_unique<QQuickWindow>();
        window_->resize(128, 64);
        window_->setColor(Qt::black);
        view_ = new VideoView(window_->contentItem());
        view_->setSize(QSizeF(128, 64));
        if (draw().isNull()) GTEST_SKIP() << "Qt Quick cannot draw here";
        if (window_->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
            GTEST_SKIP() << "Qt Quick's software renderer draws no shaders";
        }
    }

    /// `picture` handed over as a player does, from a thread of its own.
    void show(VideoFrame picture) {
        std::thread player([sink = view_->sink(), &picture] { sink(std::move(picture)); });
        player.join();
        QCoreApplication::processEvents();
    }

    QImage draw() {
        QCoreApplication::processEvents();
        return window_->grabWindow().convertToFormat(QImage::Format_RGB32);
    }

    std::unique_ptr<QQuickWindow> window_;
    VideoView* view_ = nullptr;  ///< the window's
};

TEST_F(VideoViewTest, DrawsEachLayoutAndColourSpaceInItsColours) {
    for (PixelFormat format : {PixelFormat::I420, PixelFormat::NV12}) {
        for (ColorSpace space : {ColorSpace{ColorMatrix::BT709, ColorRange::Limited},
                                 ColorSpace{ColorMatrix::BT709, ColorRange::Full},
                                 ColorSpace{ColorMatrix::BT601, ColorRange::Limited},
                                 ColorSpace{ColorMatrix::BT601, ColorRange::Full}}) {
            SCOPED_TRACE(::testing::Message()
                         << "format " << int(format) << ", matrix " << int(space.matrix)
                         << ", range " << int(space.range));
            show(quadrants(64, 64, format, space));
            const QImage image = draw();
            // A square picture in a 128 x 64 window: from x 32 to 96.
            EXPECT_TRUE(near(image.pixel(48, 16), kQuadrants[0]));
            EXPECT_TRUE(near(image.pixel(80, 16), kQuadrants[1]));
            EXPECT_TRUE(near(image.pixel(48, 48), kQuadrants[2]));
            EXPECT_TRUE(near(image.pixel(80, 48), kQuadrants[3]));
            EXPECT_TRUE(near(image.pixel(8, 32), qRgb(0, 0, 0))) << "left of the picture";
            EXPECT_TRUE(near(image.pixel(120, 32), qRgb(0, 0, 0))) << "right of the picture";
        }
    }
    EXPECT_EQ(view_->pictureSize(), QSize(64, 64));
}

TEST_F(VideoViewTest, APictureOfAnotherShapeIsFittedAnew) {
    show(quadrants(64, 64, PixelFormat::I420, {}));
    draw();
    // Tall, and of odd size: from x 48 to 80, chroma rounded up.
    show(quadrants(31, 63, PixelFormat::NV12, {}));
    const QImage image = draw();
    EXPECT_TRUE(near(image.pixel(54, 12), kQuadrants[0]));
    EXPECT_TRUE(near(image.pixel(74, 12), kQuadrants[1]));
    EXPECT_TRUE(near(image.pixel(54, 52), kQuadrants[2]));
    EXPECT_TRUE(near(image.pixel(74, 52), kQuadrants[3]));
    EXPECT_TRUE(near(image.pixel(40, 32), qRgb(0, 0, 0)));
    EXPECT_EQ(view_->pictureSize(), QSize(31, 63));
}

TEST_F(VideoViewTest, AResizedViewFitsThePictureAgain) {
    // Each grab of a window not on screen draws a scene graph of its own: the
    // picture is drawn again on the new one without a new picture to show.
    show(quadrants(64, 64, PixelFormat::I420, {}));
    draw();
    window_->resize(64, 128);
    view_->setSize(QSizeF(64, 128));
    const QImage image = draw();
    // Now from y 32 to 96.
    EXPECT_TRUE(near(image.pixel(16, 48), kQuadrants[0]));
    EXPECT_TRUE(near(image.pixel(48, 80), kQuadrants[3]));
    EXPECT_TRUE(near(image.pixel(32, 8), qRgb(0, 0, 0)));
}

TEST_F(VideoViewTest, ClearedItDrawsNothing) {
    show(quadrants(64, 64, PixelFormat::I420, {}));
    draw();
    view_->clear();
    EXPECT_TRUE(view_->pictureSize().isEmpty());
    EXPECT_TRUE(near(draw().pixel(48, 16), qRgb(0, 0, 0)));
    show(quadrants(64, 64, PixelFormat::I420, {}));
    EXPECT_TRUE(near(draw().pixel(48, 16), kQuadrants[0])) << "until the next picture";
}

TEST(VideoView, ItsSinkOutlivesIt) {
    auto view = std::make_unique<VideoView>();
    const VideoView::Sink sink = view->sink();
    view.reset();
    sink(quadrants(16, 16, PixelFormat::I420, {}));  // goes nowhere
    QCoreApplication::processEvents();
}

TEST(VideoNode, FitsAPictureKeepingItsShape) {
    EXPECT_EQ(fitted({1920, 1080}, {0, 0, 960, 1080}), QRectF(0, 270, 960, 540));
    EXPECT_EQ(fitted({640, 480}, {10, 20, 1280, 480}), QRectF(330, 20, 640, 480));
    EXPECT_EQ(fitted({}, {0, 0, 100, 100}), QRectF());
    EXPECT_EQ(fitted({100, 100}, {}), QRectF());
}

}  // namespace

int main(int argc, char** argv) {
#ifdef __linux__
    // Without a display, the platform that needs none, rather than none at all.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM") && !qEnvironmentVariableIsSet("DISPLAY") &&
        !qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
#endif
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
