// How long the engine's codecs take per picture at the sizes that matter, for
// every encoder this build of FFmpeg can open here: the number that decides
// whether a machine can stream 1080p60 at all, and which encoder preset to use.
//
// Encoders are stateful, so this runs a stream through each rather than timing
// one call in a loop: 30 pictures to settle, then 120 measured.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/video_codec.h"

using namespace ratcast;

namespace {

constexpr int kWarmup = 30;
constexpr int kMeasured = 120;

struct Size {
    uint32_t width, height, bitrate;
};

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

void run(Codec codec, const std::string& name, Size size) {
    VideoEncoderConfig config;
    config.codec = codec;
    config.encoder = name;
    config.width = size.width;
    config.height = size.height;
    config.frame_rate_num = 60;
    config.bitrate = size.bitrate;
    Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config);
    if (!encoder) return;  // no hardware for it here
    Result<std::unique_ptr<VideoDecoder>> decoder = open_video_decoder({codec, {}});
    if (!decoder) return;

    const PatternConfig pattern{(*encoder)->info().input_format, size.width, size.height, 60, 1};
    std::vector<VideoFrame> pictures;
    for (int i = 0; i < kWarmup + kMeasured; ++i) {
        pictures.push_back(
            draw_pattern(pattern, i, Timestamp{std::chrono::microseconds(i * 16'667)}));
    }

    double encode_ms = 0, decode_ms = 0, worst_encode_ms = 0;
    size_t bytes = 0;
    for (int i = 0; i < kWarmup + kMeasured; ++i) {
        std::vector<EncodedVideo> frames;
        const auto start = std::chrono::steady_clock::now();
        (void)(*encoder)->encode(pictures[i], false,
                                 [&](EncodedVideo&& v) { frames.push_back(std::move(v)); });
        const double encoded = ms_since(start);

        const auto decode_start = std::chrono::steady_clock::now();
        for (EncodedVideo& v : frames) {
            const EncodedFrame frame{v.info, std::move(v.payload).freeze()};
            (void)(*decoder)->decode(frame, [](VideoFrame&&) {});
            if (i >= kWarmup) bytes += frame.payload.size();
        }
        if (i >= kWarmup) {
            encode_ms += encoded;
            worst_encode_ms = std::max(worst_encode_ms, encoded);
            decode_ms += ms_since(decode_start);
        }
    }
    std::printf(
        "  %-12s %4ux%-4u %5.1f Mbit/s   encode %6.2f ms (worst %6.2f)   decode %6.2f ms   %6.0f "
        "B/frame\n",
        name.c_str(), size.width, size.height, size.bitrate / 1e6, encode_ms / kMeasured,
        worst_encode_ms, decode_ms / kMeasured, double(bytes) / kMeasured);
}

}  // namespace

int main() {
    const Size sizes[] = {{1280, 720, 4'000'000}, {1920, 1080, 8'000'000}};
    std::printf("Per picture, pattern content at 60 fps (%d measured after %d to settle):\n",
                kMeasured, kWarmup);
    for (Codec codec : {Codec::AV1, Codec::H265, Codec::H264}) {
        for (const std::string& name : video_encoder_candidates(codec)) {
            for (const Size& size : sizes) run(codec, name, size);
        }
    }
    return 0;
}
