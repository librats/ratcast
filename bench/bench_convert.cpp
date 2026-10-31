// BGRA into the YUV encoders take: what every captured picture costs before it
// reaches the encoder, at the sizes screens have. Next to it, FFmpeg's
// libswscale doing the same, when the build has FFmpeg. Then a camera's YUY2
// into NV12, at the sizes cameras have, and a screen's NV12 shrunk for the
// sender's preview of it (640 x 360 at most).

#include <cstdio>
#include <memory>
#include <string>

#include "framework/alloc_track.h"
#include "framework/bench.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/video_convert.h"

#ifdef RATCAST_BENCH_SWSCALE
extern "C" {
#include <libswscale/swscale.h>
}
#endif

using namespace ratcast;

namespace {

struct Size {
    const char* name;
    uint32_t width, height;
};

#ifdef RATCAST_BENCH_SWSCALE
struct SwsDeleter {
    void operator()(SwsContext* context) const noexcept { sws_freeContext(context); }
};

/// libswscale set up as the engine converts: BT.709, limited range, no scaling.
std::unique_ptr<SwsContext, SwsDeleter> swscale_for(const Size& size, AVPixelFormat format) {
    const auto w = static_cast<int>(size.width);
    const auto h = static_cast<int>(size.height);
    std::unique_ptr<SwsContext, SwsDeleter> context(
        sws_getContext(w, h, AV_PIX_FMT_BGRA, w, h, format, SWS_POINT, nullptr, nullptr, nullptr));
    if (context) {
        const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(context.get(), bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16);
    }
    return context;
}
#endif

void convert_benchmarks(bench::Bench& b, const Size& size, PixelFormat format) {
    b.group(std::string("BGRA to ") + (format == PixelFormat::NV12 ? "NV12" : "I420") + ", " +
            size.name);
    b.items(1);
    b.bytes(double(size.width) * size.height * 4);

    const VideoFrame bgra = draw_pattern({PixelFormat::BGRA, size.width, size.height}, 0, {});
    b.run("convert_frame", [&] {
        Result<VideoFrame> yuv = convert_frame(bgra, format);
        bench::do_not_optimize(yuv->plane(0).data[0]);
    });

#ifdef RATCAST_BENCH_SWSCALE
    const AVPixelFormat target = format == PixelFormat::NV12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    auto context = swscale_for(size, target);
    if (!context) return;
    b.run("libswscale", [&] {
        MutableVideoFrame out = MutableVideoFrame::allocate(format, size.width, size.height);
        const uint8_t* src[] = {bgra.plane(0).data};
        const int src_stride[] = {static_cast<int>(bgra.plane(0).stride)};
        uint8_t* dst[3] = {};
        int dst_stride[3] = {};
        for (size_t i = 0; i < plane_count(format); ++i) {
            dst[i] = out.plane(i).data;
            dst_stride[i] = static_cast<int>(out.plane(i).stride);
        }
        sws_scale(context.get(), src, src_stride, 0, static_cast<int>(size.height), dst,
                  dst_stride);
        bench::do_not_optimize(dst[0][0]);
    });
#endif
}

/// A camera's packed 4:2:2, its bytes whatever they are: the loops do not care.
void camera_benchmarks(bench::Bench& b, const Size& size) {
    b.group(std::string("YUY2 to NV12, ") + size.name);
    b.items(1);
    b.bytes(double(size.width) * size.height * 2);
    MutableVideoFrame packed =
        MutableVideoFrame::allocate(PixelFormat::YUY2, size.width, size.height);
    for (uint32_t y = 0; y < size.height; ++y) {
        uint8_t* row = packed.plane(0).data + y * packed.plane(0).stride;
        for (size_t x = 0; x < size_t{size.width} * 2; ++x) row[x] = static_cast<uint8_t>(x + y);
    }
    const VideoFrame yuy2 = std::move(packed).freeze({});
    b.run("convert_frame", [&] {
        Result<VideoFrame> nv12 = convert_frame(yuy2, PixelFormat::NV12);
        bench::do_not_optimize(nv12->plane(0).data[0]);
    });
}

/// What the sender's preview of a share costs a picture it shows.
void preview_benchmarks(bench::Bench& b, const Size& size) {
    b.group(std::string("NV12 shrunk for a preview, ") + size.name);
    b.items(1);
    b.bytes(double(size.width) * size.height * 3 / 2);
    const VideoFrame nv12 = draw_pattern({PixelFormat::NV12, size.width, size.height}, 0, {});
    const uint32_t factor = shrink_factor(size.width, size.height, 640, 360);
    b.run("shrink_frame", [&] {
        Result<VideoFrame> small = shrink_frame(nv12, factor);
        bench::do_not_optimize(small->plane(0).data[0]);
    });
}

}  // namespace

int main() {
    bench::Bench b("engine: pixel format conversion");
    for (const Size& size : {Size{"720p", 1280, 720}, Size{"1080p", 1920, 1080},
                             Size{"1440p", 2560, 1440}, Size{"4K", 3840, 2160}}) {
        for (PixelFormat format : {PixelFormat::NV12, PixelFormat::I420}) {
            convert_benchmarks(b, size, format);
        }
    }
    for (const Size& size :
         {Size{"480p", 640, 480}, Size{"720p", 1280, 720}, Size{"1080p", 1920, 1080}}) {
        camera_benchmarks(b, size);
    }
    for (const Size& size :
         {Size{"1080p", 1920, 1080}, Size{"1440p", 2560, 1440}, Size{"4K", 3840, 2160}}) {
        preview_benchmarks(b, size);
    }
    b.report();

    // Pictures come from the pool: past the first, converting allocates nothing.
    const VideoFrame bgra = draw_pattern({PixelFormat::BGRA, 1920, 1080}, 0, {});
    (void)convert_frame(bgra, PixelFormat::NV12);
    constexpr int kFrames = 100;
    track::Stats stats{};
    {
        track::Scope scope(stats);
        for (int i = 0; i < kFrames; ++i) (void)convert_frame(bgra, PixelFormat::NV12);
    }
    std::printf("\nHeap allocations per 1080p conversion, steady state: %.2f\n",
                double(stats.allocs) / kFrames);
    return 0;
}
