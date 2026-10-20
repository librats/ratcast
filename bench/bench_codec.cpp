// How long the engine's codecs take per picture at the sizes that matter, for
// every encoder this build of FFmpeg can open here: the number that decides
// whether a machine can stream 1080p60 at all, and which encoder preset to use.
//
// Encoders are stateful, so this runs a stream through each rather than timing
// one call in a loop: 30 pictures to settle, then 120 measured. Then the
// bitrate halves, as rate control changes it, and 30 more pictures show what
// that costs: the slowest of them and, for an encoder that cannot change its
// rate in flight, how many pictures on the new encoder takes over (with a
// keyframe — its size is the other cost).
//
// Then Opus, for voice and for music: microseconds per 20 ms frame both ways,
// which says what a call's sound costs next to its picture.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "ratcast/engine/audio_codec.h"
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

    (*encoder)->set_bitrate(size.bitrate / 2);
    double worst_ms = 0;
    int keyframe_at = -1;
    size_t keyframe_bytes = 0;
    for (int i = 0; i < kWarmup; ++i) {
        const auto start = std::chrono::steady_clock::now();
        (void)(*encoder)->encode(pictures[i], false, [&](EncodedVideo&& v) {
            if (v.info.keyframe && keyframe_at < 0) {
                keyframe_at = i;
                keyframe_bytes = v.payload.size();
            }
        });
        worst_ms = std::max(worst_ms, ms_since(start));
        // At the source's pace, as a live encoder gets them: the encoder being
        // opened alongside has the time in between.
        std::this_thread::sleep_until(start + std::chrono::microseconds(16'667));
    }
    if (keyframe_at < 0) {
        std::printf("  %31s bitrate halved: worst encode %6.2f ms, in flight\n", "", worst_ms);
    } else {
        std::printf(
            "  %31s bitrate halved: worst encode %6.2f ms, new encoder from picture %d, "
            "keyframe %zu B\n",
            "", worst_ms, keyframe_at, keyframe_bytes);
    }
}

constexpr int kAudioFrames = 500;  // 10 s

// Ten seconds of a chord with some noise over it: busier than a tone, which
// Opus codes in next to nothing.
AudioFrame audio_signal(AudioFormat format) {
    MutableAudioFrame frame =
        MutableAudioFrame::allocate(format, size_t{format.sample_rate} * kAudioFrames / 50);
    std::span<float> out = frame.samples();
    uint32_t noise = 1;
    for (size_t i = 0; i < frame.length(); ++i) {
        const double t = double(i) / format.sample_rate;
        double v = 0;
        for (double f : {220.0, 277.2, 329.6, 1760.0})
            v += 0.15 * std::sin(2 * std::numbers::pi * f * t);
        for (size_t c = 0; c < format.channels; ++c) {
            noise = noise * 1664525 + 1013904223;
            out[i * format.channels + c] = float(v + 0.02 * (int32_t(noise) / 2147483648.0));
        }
    }
    return std::move(frame).freeze(Timestamp{});
}

void run_audio(const char* label, const AudioEncoderConfig& config) {
    Result<std::unique_ptr<AudioEncoder>> encoder = open_audio_encoder(config);
    Result<std::unique_ptr<AudioDecoder>> decoder =
        open_audio_decoder({config.codec, config.format, {}});
    if (!encoder || !decoder) return;  // no Opus in this build

    std::vector<EncodedAudio> frames;
    const AudioFrame signal = audio_signal(config.format);
    const auto start = std::chrono::steady_clock::now();
    (void)(*encoder)->encode(signal, [&](EncodedAudio&& a) { frames.push_back(std::move(a)); });
    const double encode_ms = ms_since(start);

    size_t bytes = 0;
    const auto decode_start = std::chrono::steady_clock::now();
    for (EncodedAudio& a : frames) {
        bytes += a.payload.size();
        (void)(*decoder)->decode({a.info, std::move(a.payload).freeze()});
    }
    const double decode_ms = ms_since(decode_start);

    std::printf("  %-6s %s %3u kbit/s %u ch   encode %5.1f us   decode %5.1f us   %4.0f B/frame\n",
                label, (*encoder)->info().name.c_str(), config.bitrate / 1000,
                unsigned(config.format.channels), 1000 * encode_ms / kAudioFrames,
                1000 * decode_ms / kAudioFrames, double(bytes) / kAudioFrames);
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

    std::printf("\nPer 20 ms of sound (%d frames):\n", kAudioFrames);
    run_audio("voice", {.format = {48'000, 1}, .bitrate = 32'000, .expected_loss = 10});
    run_audio("music", {.format = {48'000, 2}, .content = AudioContent::Music, .bitrate = 128'000});
    return 0;
}
