#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

#include "ratcast/engine/pattern_source.h"

namespace ratcast {
namespace {

// ── The time stamp ──────────────────────────────────────────────────────────
//
// An 8 x 8 grid of blocks in the top-left corner, a quarter of the shorter
// side across. Block i (row-major) is bit i of a 64-bit word: 56 bits of
// capture time in microseconds, then a CRC-8 over those seven bytes.

constexpr int kGrid = 8;
constexpr int kTimeBits = 56;
constexpr uint8_t kBlack = 16;  // nominal black and white of limited-range luma
constexpr uint8_t kWhite = 235;
constexpr uint8_t kNeutral = 128;

struct StampGeometry {
    uint32_t block;  ///< side of one block in pixels
};

StampGeometry stamp_geometry(uint32_t width, uint32_t height) {
    return {std::max<uint32_t>(std::min(width, height) / 4 / kGrid, 1)};
}

uint8_t crc8(uint64_t value) {
    uint8_t crc = 0;
    for (int byte = 0; byte < kTimeBits / 8; ++byte) {
        crc ^= static_cast<uint8_t>(value >> (byte * 8));
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<uint8_t>(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
        }
    }
    return crc;
}

uint64_t stamp_word(Timestamp time) {
    const uint64_t micros =
        static_cast<uint64_t>(time.time_since_epoch().count()) & ((uint64_t{1} << kTimeBits) - 1);
    return micros | uint64_t{crc8(micros)} << kTimeBits;
}

// ── Drawing ─────────────────────────────────────────────────────────────────

/// Value v in [lo, lo + span) moving by `step` per pixel, starting at `start`.
class Ramp {
public:
    Ramp(uint32_t lo, uint32_t span, uint64_t start) : lo_(lo), span_(span), v_(start % span) {}
    uint8_t next(uint32_t step) {
        const auto value = static_cast<uint8_t>(lo_ + v_);
        v_ = (v_ + step) % span_;
        return value;
    }

private:
    uint32_t lo_, span_, v_;
};

void draw_luma(MutableVideoFrame& frame, uint8_t* plane, size_t stride, size_t pixel_step,
               uint64_t index) {
    const uint32_t w = frame.width();
    const uint32_t h = frame.height();
    const uint32_t bar_width = std::max<uint32_t>(w / 20, 1);
    const uint32_t bar_x = static_cast<uint32_t>(index * w / 120 % w);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = plane + y * stride;
        Ramp ramp(kBlack, kWhite - kBlack, uint64_t{y} * 2 + index * 6);
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t value = ramp.next(1);
            row[x * pixel_step] = (x - bar_x < bar_width) ? kWhite : value;
        }
    }
}

void draw_yuv(MutableVideoFrame& frame, uint64_t index) {
    const auto& luma = frame.plane(0);
    draw_luma(frame, luma.data, luma.stride, 1, index);

    const uint32_t cw = (frame.width() + 1) / 2;
    const uint32_t ch = (frame.height() + 1) / 2;
    for (uint32_t y = 0; y < ch; ++y) {
        Ramp u(64, 128, index * 3);
        const uint8_t v = static_cast<uint8_t>(64 + (y * 2 + index) % 128);
        if (frame.format() == PixelFormat::I420) {
            uint8_t* u_row = frame.plane(1).data + y * frame.plane(1).stride;
            uint8_t* v_row = frame.plane(2).data + y * frame.plane(2).stride;
            for (uint32_t x = 0; x < cw; ++x) {
                u_row[x] = u.next(2);
                v_row[x] = v;
            }
        } else {
            uint8_t* uv = frame.plane(1).data + y * frame.plane(1).stride;
            for (uint32_t x = 0; x < cw; ++x) {
                uv[2 * x] = u.next(2);
                uv[2 * x + 1] = v;
            }
        }
    }
}

void draw_bgra(MutableVideoFrame& frame, uint64_t index) {
    const auto& plane = frame.plane(0);
    draw_luma(frame, plane.data + 1, plane.stride, 4, index);  // green carries the luma ramp
    for (uint32_t y = 0; y < frame.height(); ++y) {
        uint8_t* row = plane.data + y * plane.stride;
        Ramp blue(64, 128, index * 3);
        const uint8_t red = static_cast<uint8_t>(64 + (y + index) % 128);
        for (uint32_t x = 0; x < frame.width(); ++x) {
            row[4 * x] = blue.next(1);
            row[4 * x + 2] = red;
            row[4 * x + 3] = 0xFF;
        }
    }
}

void draw_stamp(MutableVideoFrame& frame, Timestamp time) {
    const uint64_t word = stamp_word(time);
    const uint32_t block = stamp_geometry(frame.width(), frame.height()).block;
    const uint32_t side = block * kGrid;

    for (uint32_t y = 0; y < side; ++y) {
        for (uint32_t x = 0; x < side; ++x) {
            const int bit = static_cast<int>(y / block) * kGrid + static_cast<int>(x / block);
            const uint8_t luma = (word >> bit) & 1 ? kWhite : kBlack;
            if (frame.format() == PixelFormat::BGRA) {
                uint8_t* pixel = frame.plane(0).data + y * frame.plane(0).stride + 4 * x;
                pixel[0] = pixel[1] = pixel[2] = luma;
            } else {
                frame.plane(0).data[y * frame.plane(0).stride + x] = luma;
            }
        }
    }
    if (frame.format() == PixelFormat::BGRA) return;

    // Neutral chroma under the stamp, so a block is pure grey.
    for (uint32_t y = 0; y < side / 2; ++y) {
        if (frame.format() == PixelFormat::I420) {
            std::fill_n(frame.plane(1).data + y * frame.plane(1).stride, side / 2, kNeutral);
            std::fill_n(frame.plane(2).data + y * frame.plane(2).stride, side / 2, kNeutral);
        } else {
            std::fill_n(frame.plane(1).data + y * frame.plane(1).stride, side, kNeutral);
        }
    }
}

/// Mean luma over the middle half of block (row, col).
uint32_t block_luma(const VideoFrame& picture, uint32_t block, int row, int col) {
    const uint32_t from = block / 4;
    const uint32_t to = std::max(block - block / 4, from + 1);
    const VideoFrame::Plane& plane = picture.plane(0);
    const size_t step = picture.format() == PixelFormat::BGRA ? 4 : 1;
    const size_t offset = picture.format() == PixelFormat::BGRA ? 1 : 0;  // green

    uint32_t sum = 0;
    uint32_t count = 0;
    for (uint32_t y = row * block + from; y < row * block + to; ++y) {
        const uint8_t* line = plane.data + y * plane.stride + offset;
        for (uint32_t x = col * block + from; x < col * block + to; ++x) {
            sum += line[x * step];
            ++count;
        }
    }
    return sum / count;
}

}  // namespace

VideoFrame draw_pattern(const PatternConfig& config, uint64_t index, Timestamp capture_time) {
    assert(config.width >= 64 && config.height >= 64);
    MutableVideoFrame frame =
        MutableVideoFrame::allocate(config.format, config.width, config.height);
    if (config.format == PixelFormat::BGRA) {
        draw_bgra(frame, index);
    } else {
        draw_yuv(frame, index);
    }
    draw_stamp(frame, capture_time);
    return std::move(frame).freeze(capture_time);
}

std::optional<Timestamp> read_timestamp(const VideoFrame& picture) {
    if (picture.empty() || picture.width() < 64 || picture.height() < 64) return std::nullopt;
    const uint32_t block = stamp_geometry(picture.width(), picture.height()).block;

    uint64_t word = 0;
    for (int bit = 0; bit < kGrid * kGrid; ++bit) {
        const uint32_t luma = block_luma(picture, block, bit / kGrid, bit % kGrid);
        if (luma > (kBlack + kWhite) / 2) word |= uint64_t{1} << bit;
    }
    const uint64_t micros = word & ((uint64_t{1} << kTimeBits) - 1);
    if (static_cast<uint8_t>(word >> kTimeBits) != crc8(micros)) return std::nullopt;
    return Timestamp{Duration{static_cast<Duration::rep>(micros)}};
}

}  // namespace ratcast
