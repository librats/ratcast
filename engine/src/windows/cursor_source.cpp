// The desktop's pointer, looked at on a thread of its own (cursor_source.h).

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ratcast/engine/cursor_source.h"
#include "sleeper.h"
#include "windows/cursor_image.h"

namespace ratcast {
namespace windows {
namespace {

/// A bitmap's pixels as 32-bit BGRA, rows top first; a monochrome one's black
/// 0 and white 0xFFFFFF.
std::vector<uint32_t> pixels_of(HDC dc, HBITMAP bitmap, int width, int height) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
    if (GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS) !=
        height) {
        return {};
    }
    return pixels;
}

enum class Pixel : uint8_t { Clear, Opaque, Inverted };

}  // namespace

std::optional<CursorImage> read_cursor_image(HCURSOR cursor) {
    ICONINFO icon{};
    if (!cursor || !GetIconInfo(cursor, &icon)) return std::nullopt;
    struct Bitmaps {
        ICONINFO& icon;
        ~Bitmaps() {
            if (icon.hbmMask) DeleteObject(icon.hbmMask);
            if (icon.hbmColor) DeleteObject(icon.hbmColor);
        }
    } owned{icon};
    if (!icon.hbmMask) return std::nullopt;

    BITMAP mask{};
    if (!GetObjectW(icon.hbmMask, sizeof(mask), &mask)) return std::nullopt;
    const int width = mask.bmWidth;
    // A monochrome cursor's mask is its AND mask over its XOR mask.
    const int height = icon.hbmColor ? mask.bmHeight : mask.bmHeight / 2;
    if (width <= 0 || height <= 0 || width > static_cast<int>(kMaxCursorSide) ||
        height > static_cast<int>(kMaxCursorSide)) {
        return std::nullopt;
    }

    HDC dc = GetDC(nullptr);
    if (!dc) return std::nullopt;
    const auto masks = pixels_of(dc, icon.hbmMask, width, icon.hbmColor ? height : height * 2);
    const auto colours =
        icon.hbmColor ? pixels_of(dc, icon.hbmColor, width, height) : std::vector<uint32_t>{};
    ReleaseDC(nullptr, dc);
    if (masks.empty() || (icon.hbmColor && colours.empty())) return std::nullopt;

    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    CursorImage image{
        .width = static_cast<uint32_t>(width),
        .height = static_cast<uint32_t>(height),
        .hot_x = std::min<uint32_t>(icon.xHotspot, static_cast<uint32_t>(width - 1)),
        .hot_y = std::min<uint32_t>(icon.yHotspot, static_cast<uint32_t>(height - 1)),
        .rgba = std::vector<uint8_t>(count * 4),
    };
    auto put = [&](size_t i, uint32_t bgra, uint8_t alpha) {
        image.rgba[i * 4 + 0] = static_cast<uint8_t>(bgra >> 16);
        image.rgba[i * 4 + 1] = static_cast<uint8_t>(bgra >> 8);
        image.rgba[i * 4 + 2] = static_cast<uint8_t>(bgra);
        image.rgba[i * 4 + 3] = alpha;
    };

    bool has_alpha = false;
    for (const uint32_t c : colours) has_alpha = has_alpha || (c >> 24) != 0;
    if (has_alpha) {  // the usual cursor of today: its own alpha, the mask unused
        for (size_t i = 0; i < count; ++i)
            put(i, colours[i], static_cast<uint8_t>(colours[i] >> 24));
        return image;
    }

    // Masks: AND 1 keeps what is under the cursor, XOR then flips it.
    std::vector<Pixel> kind(count);
    for (size_t i = 0; i < count; ++i) {
        const bool keep = (masks[i] & 0xFFFFFF) != 0;
        const uint32_t colour =
            icon.hbmColor ? colours[i] & 0xFFFFFF : ((masks[count + i] & 0xFFFFFF) ? 0xFFFFFF : 0);
        if (!keep) {
            kind[i] = Pixel::Opaque;
            put(i, colour, 255);
        } else if (colour != 0) {
            kind[i] = Pixel::Inverted;
            put(i, 0, 255);
        } else {
            kind[i] = Pixel::Clear;
        }
    }
    // Inverted pixels show on any background with a white edge around them.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            if (kind[i] != Pixel::Clear) continue;
            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; ++dy) {
                for (int dx = -1; dx <= 1 && !edge; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    edge = nx >= 0 && ny >= 0 && nx < width && ny < height &&
                           kind[static_cast<size_t>(ny) * width + nx] == Pixel::Inverted;
                }
            }
            if (edge) put(i, 0xFFFFFF, 255);
        }
    }
    return image;
}

namespace {

/// FNV-1a over what makes an image look as it does.
uint64_t fingerprint(const CursorImage& image) {
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    };
    const uint32_t header[] = {image.width, image.height, image.hot_x, image.hot_y};
    mix(header, sizeof(header));
    mix(image.rgba.data(), image.rgba.size());
    return hash;
}

class WindowsCursorSource final : public CursorSource {
public:
    explicit WindowsCursorSource(const CursorSourceOptions& options)
        : period_(std::chrono::microseconds(1'000'000 / std::max<uint32_t>(options.rate, 1))) {}

    ~WindowsCursorSource() override { stop(); }

    Result<void> start(ImageHandler image, StateHandler state) override {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) return fail(Errc::InvalidArgument);
        thread_ = std::jthread([this, image = std::move(image), state = std::move(state)](
                                   std::stop_token stop) { run(stop, image, state); });
        return {};
    }

    void stop() override {
        std::lock_guard lock(mutex_);
        if (!thread_.joinable()) return;
        thread_.request_stop();
        thread_.join();
        thread_ = {};
    }

private:
    void run(std::stop_token stop, const ImageHandler& tell_image, const StateHandler& tell_state) {
        // Physical pixels, as a per-monitor aware process sees its screens,
        // whatever the process itself was made.
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Sleeper sleeper;
        std::unordered_set<uint32_t> images_told;
        HCURSOR handle = nullptr;
        uint32_t image_id = 0;
        std::optional<CursorState> told;

        auto next = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            CURSORINFO info{.cbSize = sizeof(CURSORINFO)};
            CursorState state{.visible = false};
            // Fails on a desktop this process cannot read (UAC, the lock screen).
            if (GetCursorInfo(&info)) {
                state.x = info.ptScreenPos.x;
                state.y = info.ptScreenPos.y;
                state.visible = (info.flags & CURSOR_SHOWING) != 0 && info.hCursor;
                if (state.visible && info.hCursor != handle) {
                    handle = info.hCursor;
                    image_id = 0;
                    if (auto image = read_cursor_image(handle)) {
                        const auto [known, added] = ids_.try_emplace(fingerprint(*image), next_id_);
                        if (added) ++next_id_;
                        image_id = known->second;
                        if (images_told.insert(image_id).second) {
                            image->id = image_id;
                            if (tell_image) tell_image(*image);
                        }
                    }
                }
                state.image = state.visible ? image_id : (told ? told->image : image_id);
            } else if (told) {
                state.x = told->x;
                state.y = told->y;
                state.image = told->image;
            }
            if (!told || *told != state) {
                told = state;
                if (tell_state) tell_state(state);
            }
            next += period_;
            const auto now = std::chrono::steady_clock::now();
            if (next < now) next = now;
            sleeper.until(next);
        }
    }

    const std::chrono::steady_clock::duration period_;
    // The source's thread's alone (one at a time): an image's id kept across
    // starts, so that one id never names two images.
    std::unordered_map<uint64_t, uint32_t> ids_;  // fingerprint → id
    uint32_t next_id_ = 1;
    std::mutex mutex_;
    std::jthread thread_;
};

}  // namespace
}  // namespace windows

Result<std::unique_ptr<CursorSource>> open_cursor_source(const CursorSourceOptions& options) {
    return std::unique_ptr<CursorSource>(std::make_unique<windows::WindowsCursorSource>(options));
}

}  // namespace ratcast
