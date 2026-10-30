// BGRA (and RGBA) into 4:2:0 YUV in fixed point, and a camera's 4:2:2 into 4:2:0. The loops are
// plain C++ on purpose: compilers turn them into SIMD (this file is built with -O3), which keeps
// one version of the code for every architecture and runs near memory speed.
//
// The arithmetic is 16-bit, so that a vector holds eight lanes rather than
// four: each term is the high half of a 16 x 16-bit product (what SIMD calls
// "multiply high"), with the input shifted up first so that the coefficient's
// full 16 bits of precision survive.

#include "ratcast/engine/video_convert.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace ratcast {
namespace {

/// Luma: unsigned coefficients scaled by 2^16, inputs by 2^8, so a term is in
/// 256ths of a level. Chroma: signed coefficients scaled by 2^15, inputs (sums
/// of a 2 x 2 block, scaled by 2^5) by 2^7 each, so a term is in 64ths.
constexpr int kLumaFraction = 8;
constexpr int kChromaFraction = 6;

struct Coefficients {
    uint16_t yr, yg, yb;
    uint16_t y_offset;  ///< black level and rounding, in 256ths
    int16_t ur, ug, ub;
    int16_t vr, vg, vb;
};

constexpr int32_t fixed(double value, int bits) {
    return static_cast<int32_t>(value * (1 << bits) + (value < 0 ? -0.5 : 0.5));
}

/// RGB to Y'CbCr for one matrix and range. Each row sums to what white (luma)
/// or grey (chroma) needs, so those come out exact.
constexpr Coefficients make_coefficients(double kr, double kb, bool full) {
    const double kg = 1 - kr - kb;
    const double luma_scale = full ? 1.0 : 219.0 / 255;
    const double chroma_scale = full ? 1.0 : 224.0 / 255;

    Coefficients k{};
    const int32_t yr = fixed(kr * luma_scale, 16);
    const int32_t yb = fixed(kb * luma_scale, 16);
    k.yr = static_cast<uint16_t>(yr);
    k.yb = static_cast<uint16_t>(yb);
    k.yg = static_cast<uint16_t>(fixed(luma_scale, 16) - yr - yb);
    k.y_offset =
        static_cast<uint16_t>(((full ? 0 : 16) << kLumaFraction) + (1 << (kLumaFraction - 1)));

    // Cb = (B - Y') / (2 (1 - Kb)), Cr = (R - Y') / (2 (1 - Kr)).
    const int32_t ur = fixed(-kr / (2 * (1 - kb)) * chroma_scale, 15);
    const int32_t ug = fixed(-kg / (2 * (1 - kb)) * chroma_scale, 15);
    const int32_t vg = fixed(-kg / (2 * (1 - kr)) * chroma_scale, 15);
    const int32_t vb = fixed(-kb / (2 * (1 - kr)) * chroma_scale, 15);
    k.ur = static_cast<int16_t>(ur);
    k.ug = static_cast<int16_t>(ug);
    k.ub = static_cast<int16_t>(-ur - ug);
    k.vg = static_cast<int16_t>(vg);
    k.vb = static_cast<int16_t>(vb);
    k.vr = static_cast<int16_t>(-vg - vb);
    return k;
}

constexpr Coefficients kBt709Limited = make_coefficients(0.2126, 0.0722, false);
constexpr Coefficients kBt709Full = make_coefficients(0.2126, 0.0722, true);
constexpr Coefficients kBt601Limited = make_coefficients(0.299, 0.114, false);
constexpr Coefficients kBt601Full = make_coefficients(0.299, 0.114, true);

const Coefficients& coefficients(ColorSpace color_space) {
    const bool full = color_space.range == ColorRange::Full;
    if (color_space.matrix == ColorMatrix::BT601) return full ? kBt601Full : kBt601Limited;
    return full ? kBt709Full : kBt709Limited;
}

/// The same for RGBA, read by BGRA's loops: red and blue trade places.
Coefficients swapped(Coefficients k) {
    std::swap(k.yr, k.yb);
    std::swap(k.ur, k.ub);
    std::swap(k.vr, k.vb);
    return k;
}

/// The high halves of 16 x 16-bit products, written the way vectorisers
/// recognise.
inline uint16_t mul_high(uint16_t a, uint16_t b) {
    return static_cast<uint16_t>((uint32_t{a} * b) >> 16);
}
inline int16_t mul_high(int16_t a, int16_t b) {
    return static_cast<int16_t>((int32_t{a} * b) >> 16);
}

// What lets the vectoriser in: size_t indices (a uint32_t 4 * x may wrap, so
// it could not prove the access strided), and coefficients by value (through a
// reference, any store to a byte might change them).
void luma_row(const uint8_t* __restrict bgra, uint8_t* __restrict y, size_t width,
              const Coefficients k) {
    for (size_t x = 0; x < width; ++x) {
        const uint8_t* p = bgra + 4 * x;
        const auto term = [](uint8_t value, uint16_t c) {
            return mul_high(static_cast<uint16_t>(value << 8), c);
        };
        const auto sum = static_cast<uint16_t>(term(p[2], k.yr) + term(p[1], k.yg) +
                                               term(p[0], k.yb) + k.y_offset);
        y[x] = static_cast<uint8_t>(sum >> kLumaFraction);
    }
}

/// One chroma sample from the sums of a 2 x 2 block's channels, each scaled
/// by 2^5. Full-range chroma can round up to 256, hence the clamp.
inline uint8_t chroma(int16_t cr, int16_t cg, int16_t cb, int16_t r, int16_t g, int16_t b) {
    const int32_t sum = mul_high(r, cr) + mul_high(g, cg) + mul_high(b, cb);
    constexpr int32_t kOffset = (128 << kChromaFraction) + (1 << (kChromaFraction - 1));
    return static_cast<uint8_t>(std::min((sum + kOffset) >> kChromaFraction, 255));
}

/// Chroma for the rows `a` and `b` (the same row at the bottom of an odd
/// height): into `u` and `v`, or interleaved into `u` alone.
template <bool kInterleaved>
void chroma_row(const uint8_t* __restrict a, const uint8_t* __restrict b, uint8_t* __restrict u,
                uint8_t* __restrict v, size_t width, const Coefficients k) {
    const auto store = [&](size_t x, int32_t r, int32_t g, int32_t bl) {
        const auto scaled = [](int32_t sum) { return static_cast<int16_t>(sum << 5); };
        const uint8_t cb = chroma(k.ur, k.ug, k.ub, scaled(r), scaled(g), scaled(bl));
        const uint8_t cr = chroma(k.vr, k.vg, k.vb, scaled(r), scaled(g), scaled(bl));
        if constexpr (kInterleaved) {
            u[2 * x] = cb;
            u[2 * x + 1] = cr;
        } else {
            u[x] = cb;
            v[x] = cr;
        }
    };

    const size_t pairs = width / 2;
    for (size_t x = 0; x < pairs; ++x) {
        const uint8_t* p = a + 8 * x;
        const uint8_t* q = b + 8 * x;
        store(x, p[2] + p[6] + q[2] + q[6], p[1] + p[5] + q[1] + q[5], p[0] + p[4] + q[0] + q[4]);
    }
    if (width % 2 != 0) {  // a last column of its own counts twice
        const uint8_t* p = a + 8 * pairs;
        const uint8_t* q = b + 8 * pairs;
        store(pairs, 2 * (p[2] + q[2]), 2 * (p[1] + q[1]), 2 * (p[0] + q[0]));
    }
}

void bgra_to_yuv(const VideoFrame& in, MutableVideoFrame& out, const Coefficients& k) {
    const uint32_t width = in.width();
    const uint32_t height = in.height();
    const VideoFrame::Plane& src = in.plane(0);
    const MutableVideoFrame::Plane& luma = out.plane(0);

    // Two rows at a time, chroma while they are still in cache.
    for (uint32_t y = 0; y < height; y += 2) {
        const uint8_t* a = src.data + y * src.stride;
        luma_row(a, luma.data + y * luma.stride, width, k);
        const uint8_t* b = a;  // the last row of an odd height pairs with itself
        if (y + 1 < height) {
            b = a + src.stride;
            luma_row(b, luma.data + (y + 1) * luma.stride, width, k);
        }

        const size_t line = y / 2;
        if (out.format() == PixelFormat::NV12) {
            uint8_t* uv = out.plane(1).data + line * out.plane(1).stride;
            chroma_row<true>(a, b, uv, nullptr, width, k);
        } else {
            uint8_t* u = out.plane(1).data + line * out.plane(1).stride;
            uint8_t* v = out.plane(2).data + line * out.plane(2).stride;
            chroma_row<false>(a, b, u, v, width, k);
        }
    }
}

void copy_planes(const VideoFrame& in, MutableVideoFrame& out) {
    for (size_t i = 0; i < in.plane_count(); ++i) {
        const size_t bytes = plane_row_bytes(in.format(), i, in.width());
        for (size_t y = 0; y < plane_rows(in.format(), i, in.height()); ++y) {
            std::memcpy(out.plane(i).data + y * out.plane(i).stride,
                        in.plane(i).data + y * in.plane(i).stride, bytes);
        }
    }
}

/// The luma plane as it is, the chroma planes interleaved (I420 into NV12) or
/// split apart (NV12 into I420).
void relayout_yuv(const VideoFrame& in, MutableVideoFrame& out) {
    const size_t width = in.width();
    for (size_t y = 0; y < in.height(); ++y) {
        std::memcpy(out.plane(0).data + y * out.plane(0).stride,
                    in.plane(0).data + y * in.plane(0).stride, width);
    }
    const size_t chroma_width = (width + 1) / 2;
    for (size_t y = 0; y < plane_rows(in.format(), 1, in.height()); ++y) {
        if (in.format() == PixelFormat::I420) {
            const uint8_t* __restrict u = in.plane(1).data + y * in.plane(1).stride;
            const uint8_t* __restrict v = in.plane(2).data + y * in.plane(2).stride;
            uint8_t* __restrict uv = out.plane(1).data + y * out.plane(1).stride;
            for (size_t x = 0; x < chroma_width; ++x) {
                uv[2 * x] = u[x];
                uv[2 * x + 1] = v[x];
            }
        } else {
            const uint8_t* __restrict uv = in.plane(1).data + y * in.plane(1).stride;
            uint8_t* __restrict u = out.plane(1).data + y * out.plane(1).stride;
            uint8_t* __restrict v = out.plane(2).data + y * out.plane(2).stride;
            for (size_t x = 0; x < chroma_width; ++x) {
                u[x] = uv[2 * x];
                v[x] = uv[2 * x + 1];
            }
        }
    }
}

/// 4:2:2 packed into 4:2:0: luma as it is, chroma the mean of two rows'.
void yuy2_to_yuv(const VideoFrame& in, MutableVideoFrame& out) {
    const size_t pairs = (size_t{in.width()} + 1) / 2;
    const size_t width = in.width();
    const VideoFrame::Plane& src = in.plane(0);
    for (uint32_t y = 0; y < in.height(); y += 2) {
        const uint8_t* __restrict a = src.data + y * src.stride;
        const uint8_t* __restrict b = y + 1 < in.height() ? a + src.stride : a;
        uint8_t* __restrict luma_a = out.plane(0).data + y * out.plane(0).stride;
        for (size_t x = 0; x < width; ++x) luma_a[x] = a[2 * x];
        if (b != a) {
            uint8_t* __restrict luma_b = luma_a + out.plane(0).stride;
            for (size_t x = 0; x < width; ++x) luma_b[x] = b[2 * x];
        }
        const auto mean = [](uint8_t p, uint8_t q) { return uint8_t((p + q + 1) / 2); };
        const size_t line = y / 2;
        if (out.format() == PixelFormat::NV12) {
            uint8_t* __restrict uv = out.plane(1).data + line * out.plane(1).stride;
            for (size_t x = 0; x < 2 * pairs; ++x) uv[x] = mean(a[2 * x + 1], b[2 * x + 1]);
        } else {
            uint8_t* __restrict u = out.plane(1).data + line * out.plane(1).stride;
            uint8_t* __restrict v = out.plane(2).data + line * out.plane(2).stride;
            for (size_t x = 0; x < pairs; ++x) {
                u[x] = mean(a[4 * x + 1], b[4 * x + 1]);
                v[x] = mean(a[4 * x + 3], b[4 * x + 3]);
            }
        }
    }
}

bool is_yuv(PixelFormat format) {
    return format != PixelFormat::BGRA && format != PixelFormat::RGBA;
}
bool is_420(PixelFormat format) {
    return format == PixelFormat::I420 || format == PixelFormat::NV12;
}

}  // namespace

Result<VideoFrame> convert_frame(const VideoFrame& picture, PixelFormat format,
                                 ColorSpace color_space, BufferPool& pool) {
    if (picture.empty()) return fail(Errc::InvalidArgument);
    if (picture.format() != format && !is_420(format)) return fail(Errc::Unsupported);
    if (picture.gpu()) {  // read out of GPU memory first
        Result<VideoFrame> downloaded = download_frame(picture, pool);
        if (!downloaded) return downloaded;
        if (downloaded->format() == format) return downloaded;
        return convert_frame(*downloaded, format, color_space, pool);
    }

    MutableVideoFrame out =
        MutableVideoFrame::allocate(format, picture.width(), picture.height(), pool);
    if (picture.format() == format) {
        copy_planes(picture, out);
    } else if (picture.format() == PixelFormat::YUY2) {
        yuy2_to_yuv(picture, out);
    } else if (is_yuv(picture.format())) {
        relayout_yuv(picture, out);
    } else {
        const Coefficients& k = coefficients(color_space);
        bgra_to_yuv(picture, out, picture.format() == PixelFormat::RGBA ? swapped(k) : k);
        return std::move(out).freeze(picture.capture_time(), color_space);
    }
    return std::move(out).freeze(picture.capture_time(), picture.color_space());
}

Result<VideoFrame> convert_frame(const VideoFrame& picture, PixelFormat format,
                                 ColorSpace color_space) {
    return convert_frame(picture, format, color_space, BufferPool::shared());
}

YuvToRgb rgb_from_yuv(ColorSpace color_space) noexcept {
    const bool bt709 = color_space.matrix == ColorMatrix::BT709;
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double kg = 1 - kr - kb;
    // Y' = luma_scale · Y + luma_offset, and the same for Cb and Cr about 0.
    const bool full = color_space.range == ColorRange::Full;
    const double luma_scale = full ? 1.0 : 255.0 / 219;
    const double luma_offset = full ? 0.0 : -16.0 / 219;
    const double chroma_scale = full ? 1.0 : 255.0 / 224;
    const double chroma_offset = full ? -128.0 / 255 : -128.0 / 224;

    // R = Y' + 2 (1 - Kr) Cr, B = Y' + 2 (1 - Kb) Cb, and G what is left of Y'.
    const double r_cr = 2 * (1 - kr);
    const double b_cb = 2 * (1 - kb);
    const double g_cb = -kb * b_cb / kg;
    const double g_cr = -kr * r_cr / kg;
    const auto row = [&](double cb, double cr) {
        return std::array{float(luma_scale), float(cb * chroma_scale), float(cr * chroma_scale),
                          float(luma_offset + (cb + cr) * chroma_offset)};
    };
    return {row(0, r_cr), row(g_cb, g_cr), row(b_cb, 0)};
}

}  // namespace ratcast
