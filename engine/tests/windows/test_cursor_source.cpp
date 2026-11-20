// The desktop's pointer apart from its pictures: cursors of the test's own
// making read into RGBA, and the machine's own pointer followed.

#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <vector>

#include "ratcast/engine/cursor_source.h"
#include "windows/cursor_image.h"

namespace ratcast {
namespace {

std::vector<uint8_t> pixel_at(const CursorImage& image, uint32_t x, uint32_t y) {
    const size_t at = (static_cast<size_t>(y) * image.width + x) * 4;
    return {image.rgba.begin() + at, image.rgba.begin() + at + 4};
}

/// A cursor of 32-bit colour with alpha, every pixel `bgra` but (1, 2).
HCURSOR alpha_cursor(int side, uint32_t bgra, uint32_t odd_one) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = side;
    header.bV5Height = -side;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP colour = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS,
                                      &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    auto* pixels = static_cast<uint32_t*>(bits);
    for (int i = 0; i < side * side; ++i) pixels[i] = bgra;
    pixels[2 * side + 1] = odd_one;
    HBITMAP mask = CreateBitmap(side, side, 1, 1, nullptr);
    ICONINFO info{
        .fIcon = FALSE, .xHotspot = 3, .yHotspot = 4, .hbmMask = mask, .hbmColor = colour};
    HCURSOR cursor = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(colour);
    return cursor;
}

TEST(CursorImage, AColourCursorKeepsItsAlphaAndHotspot) {
    HCURSOR cursor = alpha_cursor(32, 0x80FF2010, 0xFF00FF00);
    ASSERT_NE(cursor, nullptr);
    const auto image = windows::read_cursor_image(cursor);
    DestroyCursor(cursor);
    ASSERT_TRUE(image);
    EXPECT_EQ(image->width, 32u);
    EXPECT_EQ(image->height, 32u);
    EXPECT_EQ(image->hot_x, 3u);
    EXPECT_EQ(image->hot_y, 4u);
    EXPECT_EQ(image->rgba.size(), 32u * 32u * 4u);
    EXPECT_EQ(pixel_at(*image, 0, 0), (std::vector<uint8_t>{0xFF, 0x20, 0x10, 0x80}));
    EXPECT_EQ(pixel_at(*image, 1, 2), (std::vector<uint8_t>{0x00, 0xFF, 0x00, 0xFF}));
}

TEST(CursorImage, AMonochromeCursorIsBlackAndWhiteItsInvertedPixelsEdged) {
    // 16 x 16, a row of two bytes in each plane. AND 1 keeps the screen, XOR
    // flips it: (0,0) black, (1,0) white, (2,0) clear, (8,8) inverted.
    constexpr int kSide = 16;
    std::vector<uint8_t> and_plane(kSide * 2, 0xFF);
    std::vector<uint8_t> xor_plane(kSide * 2, 0x00);
    and_plane[0] = 0x3F;          // (0,0) and (1,0) drawn
    xor_plane[0] = 0x40;          // (1,0) white
    xor_plane[8 * 2 + 1] = 0x80;  // (8,8) flipped, AND left 1
    HCURSOR cursor = CreateCursor(GetModuleHandleW(nullptr), 0, 0, kSide, kSide, and_plane.data(),
                                  xor_plane.data());
    ASSERT_NE(cursor, nullptr);
    const auto image = windows::read_cursor_image(cursor);
    DestroyCursor(cursor);
    ASSERT_TRUE(image);
    EXPECT_EQ(image->width, 16u);
    EXPECT_EQ(image->height, 16u);
    EXPECT_EQ(pixel_at(*image, 0, 0), (std::vector<uint8_t>{0, 0, 0, 255}));
    EXPECT_EQ(pixel_at(*image, 1, 0), (std::vector<uint8_t>{255, 255, 255, 255}));
    EXPECT_EQ(pixel_at(*image, 2, 0)[3], 0);
    EXPECT_EQ(pixel_at(*image, 8, 8), (std::vector<uint8_t>{0, 0, 0, 255}));
    EXPECT_EQ(pixel_at(*image, 7, 7), (std::vector<uint8_t>{255, 255, 255, 255}));
    EXPECT_EQ(pixel_at(*image, 9, 8), (std::vector<uint8_t>{255, 255, 255, 255}));
    EXPECT_EQ(pixel_at(*image, 10, 8)[3], 0);
}

TEST(CursorImage, TheSystemsCursorsAllRead) {
    for (const auto* name : {IDC_ARROW, IDC_IBEAM, IDC_WAIT, IDC_CROSS, IDC_HAND, IDC_SIZEALL}) {
        const auto image = windows::read_cursor_image(LoadCursor(nullptr, name));
        ASSERT_TRUE(image);
        EXPECT_GT(image->width, 0u);
        EXPECT_LT(image->hot_x, image->width);
        EXPECT_LT(image->hot_y, image->height);
        bool any_drawn = false;
        for (size_t i = 3; i < image->rgba.size(); i += 4) any_drawn = any_drawn || image->rgba[i];
        EXPECT_TRUE(any_drawn);
    }
}

/// The calling thread in physical pixels, as the source tells the pointer, for
/// as long as it lives; then as it was, so that the tests after are not changed.
class PhysicalPixels {
public:
    PhysicalPixels()
        : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalPixels() {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }
    PhysicalPixels(const PhysicalPixels&) = delete;
    PhysicalPixels& operator=(const PhysicalPixels&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous_;
};

/// What a source told, in order.
struct Heard {
    std::mutex mutex;
    std::condition_variable changed;
    std::set<uint32_t> images;
    std::vector<CursorState> states;
    bool image_before_its_state = true;

    template <typename Predicate>
    bool wait_for(Predicate predicate) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(3), [&] { return predicate(*this); });
    }
};

TEST(CursorSource, FollowsThePointerAndToldEachImageBeforeItsState) {
    const PhysicalPixels physical;
    // Before the source: one still running when a failed assertion returns
    // tells what it sees to what it was given until it is gone.
    Heard heard;
    auto source = open_cursor_source({.rate = 200});
    ASSERT_TRUE(source) << to_string(source.error());

    POINT before{};
    GetCursorPos(&before);
    ASSERT_TRUE((*source)->start(
        [&](const CursorImage& image) {
            std::lock_guard lock(heard.mutex);
            EXPECT_NE(image.id, 0u);
            EXPECT_TRUE(heard.images.insert(image.id).second) << "image " << image.id << " twice";
            EXPECT_EQ(image.rgba.size(), size_t{image.width} * image.height * 4);
        },
        [&](const CursorState& state) {
            std::lock_guard lock(heard.mutex);
            if (state.image != 0 && !heard.images.contains(state.image)) {
                heard.image_before_its_state = false;
            }
            heard.states.push_back(state);
            heard.changed.notify_all();
        }));
    EXPECT_TRUE(heard.wait_for([](Heard& h) { return !h.states.empty(); })) << "no state at start";

    const HMONITOR monitor = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{.cbSize = sizeof(MONITORINFO)};
    GetMonitorInfoW(monitor, &info);
    const int x = info.rcMonitor.left + 123;
    const int y = info.rcMonitor.top + 77;
    ASSERT_TRUE(SetCursorPos(x, y));
    const bool moved = heard.wait_for([&](Heard& h) {
        return !h.states.empty() && h.states.back().x == x && h.states.back().y == y;
    });
    (*source)->stop();
    SetCursorPos(before.x, before.y);

    std::lock_guard lock(heard.mutex);
    EXPECT_TRUE(moved) << "last told " << heard.states.back().x << "," << heard.states.back().y;
    EXPECT_TRUE(heard.image_before_its_state);
    for (size_t i = 1; i < heard.states.size(); ++i) {
        EXPECT_NE(heard.states[i], heard.states[i - 1]) << "a state told twice";
    }
}

TEST(CursorSource, StopsAndStartsAgainTellingItsImagesAfresh) {
    auto source = open_cursor_source();
    ASSERT_TRUE(source);
    for (int round = 0; round < 2; ++round) {
        Heard heard;
        ASSERT_TRUE((*source)->start(
            [&](const CursorImage& image) {
                std::lock_guard lock(heard.mutex);
                heard.images.insert(image.id);
            },
            [&](const CursorState& state) {
                std::lock_guard lock(heard.mutex);
                if (state.image != 0 && !heard.images.contains(state.image)) {
                    heard.image_before_its_state = false;
                }
                heard.states.push_back(state);
                heard.changed.notify_all();
            }));
        EXPECT_FALSE((*source)->start({}, {})) << "started twice";
        EXPECT_TRUE(heard.wait_for([](Heard& h) { return !h.states.empty(); }));
        (*source)->stop();
        std::lock_guard lock(heard.mutex);
        EXPECT_TRUE(heard.image_before_its_state) << "round " << round;
    }
}

}  // namespace
}  // namespace ratcast
