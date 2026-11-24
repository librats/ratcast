// How long the engine's codecs take per picture at the sizes that matter, for
// every encoder this build of FFmpeg can open here: the number that decides
// whether a machine can stream 1080p60 at all, and which encoder preset to use.
//
// Encoders are stateful, so this runs a stream through each rather than timing
// one call in a loop: 30 pictures to settle, then 120 measured. Then the
// bitrate halves, as rate control changes it, and 30 more pictures show what
// that costs: the slowest of them and, for an encoder that cannot change its
// rate in flight, how many pictures on the new encoder takes over (with a
// keyframe — its size is the other cost). Where a decoder hands pictures out
// in GPU memory when asked (Android's), the same frames through one so asked
// too: what decoding for a view that draws them there costs.
//
// Then encoding alone at a screen's sizes (1080, 1440 and 2160 lines), as a
// share does it: back to back, and at its pace of 60 a second, when between
// pictures a GPU may clock down and each then takes longer. From memory, and
// on Windows from Direct3D 11's memory too, as a screen captured comes.
//
// Then Opus, for voice and for music: microseconds per 20 ms frame both ways,
// which says what a call's sound costs next to its picture.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "ratcast/engine/audio_codec.h"
#include "ratcast/engine/pattern_source.h"
#include "ratcast/engine/video_codec.h"
#include "sleeper.h"

#ifdef _WIN32
#include "windows/d3d11.h"
#include "windows/nv12_converter.h"
#endif

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
    Result<std::unique_ptr<VideoDecoder>> gpu_decoder =
        open_video_decoder({.codec = codec, .codec_config = {}, .gpu = true});

    const PatternConfig pattern{(*encoder)->info().input_format, size.width, size.height, 60, 1};
    std::vector<VideoFrame> pictures;
    for (int i = 0; i < kWarmup + kMeasured; ++i) {
        pictures.push_back(
            draw_pattern(pattern, i, Timestamp{std::chrono::microseconds(i * 16'667)}));
    }

    double encode_ms = 0, decode_ms = 0, gpu_decode_ms = 0, worst_encode_ms = 0;
    bool in_gpu = false;  // whether the decoder asked for GPU pictures gave them
    size_t bytes = 0;
    for (int i = 0; i < kWarmup + kMeasured; ++i) {
        std::vector<EncodedVideo> frames;
        const auto start = std::chrono::steady_clock::now();
        (void)(*encoder)->encode(pictures[i], false,
                                 [&](EncodedVideo&& v) { frames.push_back(std::move(v)); });
        const double encoded = ms_since(start);

        std::vector<EncodedFrame> encoded_frames;
        for (EncodedVideo& v : frames)
            encoded_frames.push_back({v.info, std::move(v.payload).freeze()});
        const auto decode_start = std::chrono::steady_clock::now();
        for (const EncodedFrame& frame : encoded_frames) {
            (void)(*decoder)->decode(frame, [](VideoFrame&&) {});
            if (i >= kWarmup) bytes += frame.payload.size();
        }
        const double decoded = ms_since(decode_start);
        const auto gpu_start = std::chrono::steady_clock::now();
        for (const EncodedFrame& frame : encoded_frames) {
            if (gpu_decoder) {
                (void)(*gpu_decoder)->decode(frame, [&](VideoFrame&& p) {
                    in_gpu = in_gpu || p.gpu() != nullptr;
                });
            }
        }
        if (i >= kWarmup) {
            encode_ms += encoded;
            worst_encode_ms = std::max(worst_encode_ms, encoded);
            decode_ms += decoded;
            gpu_decode_ms += ms_since(gpu_start);
        }
    }

    std::printf(
        "  %-12s %4ux%-4u %5.1f Mbit/s   encode %6.2f ms (worst %6.2f)   decode %6.2f ms   %6.0f "
        "B/frame\n",
        name.c_str(), size.width, size.height, size.bitrate / 1e6, encode_ms / kMeasured,
        worst_encode_ms, decode_ms / kMeasured, double(bytes) / kMeasured);
    if (in_gpu)
        std::printf("  %31s decode into GPU memory %6.2f ms\n", "", gpu_decode_ms / kMeasured);

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

/// Pictures drawn for the screen sizes, over and over: a 4K stream's worth
/// would not fit a GPU's memory.
constexpr int kDistinct = 30;

/// Encoding alone at a screen's size: back to back, then at 60 a second.
/// `place` puts each picture where the encoder reads it from (as it is, or
/// into the memory of `device`, which the encoder is then opened on).
void run_screen(Codec codec, const std::string& name, Size size, const char* where,
                std::shared_ptr<GpuDevice> device,
                const std::function<VideoFrame(const VideoFrame&)>& place) {
    VideoEncoderConfig config;
    config.codec = codec;
    config.encoder = name;
    config.width = size.width;
    config.height = size.height;
    config.frame_rate_num = 60;
    config.bitrate = size.bitrate;
    config.device = device;
    Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config);
    if (!encoder) return;                                 // no hardware for it here
    if (device && !(*encoder)->info().gpu_input) return;  // it would download them

    const PixelFormat format = device ? PixelFormat::NV12 : (*encoder)->info().input_format;
    std::vector<VideoFrame> pictures;
    for (int i = 0; i < kDistinct; ++i) {
        pictures.push_back(place(draw_pattern({format, size.width, size.height, 60, 1}, i, {})));
        if (pictures.back().empty()) return;
    }

    uint64_t n = 0;
    const auto encode = [&] {
        VideoFrame picture = pictures[n % kDistinct];
        picture.set_capture_time(Timestamp{std::chrono::microseconds(n * 16'667)});
        ++n;
        const auto start = std::chrono::steady_clock::now();
        (void)(*encoder)->encode(picture, false, [](EncodedVideo&&) {});
        return ms_since(start);
    };
    for (int i = 0; i < kWarmup; ++i) (void)encode();
    double back_to_back_ms = 0;
    for (int i = 0; i < kMeasured; ++i) back_to_back_ms += encode();

    Sleeper sleeper;  // Sleep() steps in 15.6 ms on Windows
    double paced_ms = 0, worst_ms = 0;
    for (int i = 0; i < kMeasured; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const double ms = encode();
        paced_ms += ms;
        worst_ms = std::max(worst_ms, ms);
        sleeper.until(start + std::chrono::microseconds(16'667));
    }
    std::printf("  %-12s %4ux%-4u %-6s  back to back %6.2f ms   at 60 fps %6.2f ms (worst %6.2f)\n",
                name.c_str(), size.width, size.height, where, back_to_back_ms / kMeasured,
                paced_ms / kMeasured, worst_ms);
}

#ifdef _WIN32
/// As a screen captured comes on Windows: BGRA textures of `screen`'s size
/// turned into NV12 of `out`'s by the GPU's video processor (made smaller
/// when it is less), then encoded, at 60 a second. What the conversion takes
/// on the GPU (waited for), what encoding takes after it, and what encoding
/// the same pictures takes with no conversion before it (read back and
/// uploaded): content a gradient that moves, which costs NVENC more than the
/// pattern does.
void run_captured(Codec codec, const std::string& name, Size screen, Size out,
                  const std::shared_ptr<d3d11::Device>& device) {
    Result<std::unique_ptr<d3d11::Nv12Converter>> converter =
        d3d11::Nv12Converter::create(device, ColorSpace{});
    if (!converter) return;

    // Gradients that differ from one to the next, as moving content would.
    std::vector<win::ComPtr<ID3D11Texture2D>> textures;
    std::vector<uint32_t> pixels(size_t{screen.width} * screen.height);
    for (int i = 0; i < kDistinct; ++i) {
        for (uint32_t y = 0; y < screen.height; ++y) {
            for (uint32_t x = 0; x < screen.width; ++x) {
                const uint32_t v = (x + y * 3 + uint32_t(i) * 37) & 0xff;
                pixels[size_t{y} * screen.width + x] =
                    0xff000000u | (v << 16) | (((x ^ y) & 0xff) << 8) | ((y + i) & 0xff);
            }
        }
        const D3D11_TEXTURE2D_DESC desc{screen.width,
                                        screen.height,
                                        1,
                                        1,
                                        DXGI_FORMAT_B8G8R8A8_UNORM,
                                        {1, 0},
                                        D3D11_USAGE_DEFAULT,
                                        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                                        0,
                                        0};
        const D3D11_SUBRESOURCE_DATA data{pixels.data(), screen.width * 4, 0};
        win::ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->get()->CreateTexture2D(&desc, &data, &texture))) return;
        textures.push_back(std::move(texture));
    }

    uint64_t n = 0;
    const auto convert = [&] {
        Result<VideoFrame> picture =
            (*converter)
                ->convert(*textures[n % kDistinct].Get(), screen.width, screen.height, out.width,
                          out.height, Timestamp{std::chrono::microseconds(n * 16'667)});
        ++n;
        return picture ? std::move(*picture) : VideoFrame{};
    };
    const VideoFrame first = convert();
    if (first.empty()) return;

    VideoEncoderConfig config;
    config.codec = codec;
    config.encoder = name;
    config.width = first.width();
    config.height = first.height();
    config.frame_rate_num = 60;
    config.bitrate = out.bitrate;
    config.color_space = first.color_space();
    config.device = device;
    Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config);
    if (!encoder || !(*encoder)->info().gpu_input) return;

    // Waiting for the GPU to have done what it was given: what the
    // conversion takes there, apart from the encoder.
    const D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
    win::ComPtr<ID3D11Query> query;
    if (FAILED(device->get()->CreateQuery(&query_desc, &query))) return;
    const auto gpu_done = [&] {
        const d3d11::Lock lock(*device);
        device->context()->End(query.Get());
        BOOL done = FALSE;
        while (device->context()->GetData(query.Get(), &done, sizeof(done), 0) != S_OK || !done) {}
    };

    // Each picture's (picture, true) or (an uploaded one, false) through the
    // encoder at 60 a second: what getting it took and encoding it.
    Sleeper sleeper;
    struct Times {
        double get_ms = 0, encode_ms = 0, worst_ms = 0;
    };
    const auto measure = [&](const std::function<VideoFrame(int)>& get) {
        Times times;
        for (int i = 0; i < kWarmup + kMeasured; ++i) {
            const auto start = std::chrono::steady_clock::now();
            const VideoFrame picture = get(i);
            const double got = ms_since(start);
            const auto encode_start = std::chrono::steady_clock::now();
            (void)(*encoder)->encode(picture, false, [](EncodedVideo&&) {});
            const double encoded = ms_since(encode_start);
            if (i >= kWarmup) {
                times.get_ms += got / kMeasured;
                times.encode_ms += encoded / kMeasured;
                times.worst_ms = std::max(times.worst_ms, encoded);
            }
            sleeper.until(start + std::chrono::microseconds(16'667));
        }
        return times;
    };
    const Times converted = measure([&](int) {
        VideoFrame picture = convert();
        gpu_done();
        return picture;
    });

    std::vector<VideoFrame> same;
    for (int i = 0; i < kDistinct; ++i) {
        const VideoFrame picture = convert();
        MutableVideoFrame read =
            MutableVideoFrame::allocate(PixelFormat::NV12, picture.width(), picture.height());
        if (picture.empty() || !picture.gpu()->read(read)) return;
        Result<VideoFrame> uploaded =
            d3d11::upload(device, std::move(read).freeze(picture.capture_time()));
        if (!uploaded) return;
        same.push_back(std::move(*uploaded));
    }
    const Times uploaded = measure([&](int i) {
        VideoFrame picture = same[i % kDistinct];
        picture.set_capture_time(Timestamp{std::chrono::microseconds((n++) * 16'667)});
        return picture;
    });

    std::printf(
        "  %-12s %4ux%-4u into %4ux%-4u   GPU conversion %5.2f ms   encode %6.2f ms "
        "(worst %6.2f)   the same, uploaded %6.2f ms (worst %6.2f)\n",
        name.c_str(), screen.width, screen.height, config.width, config.height, converted.get_ms,
        converted.encode_ms, converted.worst_ms, uploaded.encode_ms, uploaded.worst_ms);
}
#endif

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

    // At bitrate_for_size()'s for 60 a second.
    const Size screens[] = {
        {1920, 1080, 8'000'000}, {2560, 1440, 12'300'000}, {3840, 2160, 22'600'000}};
#ifdef _WIN32
    Result<std::shared_ptr<d3d11::Device>> gpu = d3d11::Device::create();
    if (gpu && !(*gpu)->video()) gpu = fail(Errc::Unavailable);
#endif
    std::printf("\nEncoding alone at a screen's size, %d pictures each way:\n", kMeasured);
    for (Codec codec : {Codec::AV1, Codec::H265, Codec::H264}) {
        for (const std::string& name : video_encoder_candidates(codec)) {
            for (const Size& size : screens) {
                run_screen(codec, name, size, "memory", nullptr,
                           [](const VideoFrame& picture) { return picture; });
#ifdef _WIN32
                if (gpu) {
                    run_screen(codec, name, size, "D3D11", *gpu, [&](const VideoFrame& picture) {
                        Result<VideoFrame> uploaded = d3d11::upload(*gpu, picture);
                        return uploaded ? std::move(*uploaded) : VideoFrame{};
                    });
                }
#endif
            }
        }
    }

#ifdef _WIN32
    if (gpu) {
        // A 16:10 screen of 3840 x 2400 shared at 1440 lines, among others.
        const std::pair<Size, Size> captures[] = {
            {{1920, 1080, 0}, {1920, 1080, 8'000'000}},
            {{2560, 1440, 0}, {2560, 1440, 12'300'000}},
            {{3840, 2160, 0}, {3840, 2160, 22'600'000}},
            {{3840, 2160, 0}, {2560, 1440, 12'300'000}},
            {{3840, 2400, 0}, {2304, 1440, 11'400'000}},
        };
        std::printf("\nA screen captured: converted by the GPU, then encoded, at 60 fps:\n");
        for (Codec codec : {Codec::AV1, Codec::H265, Codec::H264}) {
            for (const std::string& name : video_encoder_candidates(codec)) {
                for (const auto& [screen, out] : captures)
                    run_captured(codec, name, screen, out, *gpu);
            }
        }
    }
#endif

    std::printf("\nPer 20 ms of sound (%d frames):\n", kAudioFrames);
    run_audio("voice", {.format = {48'000, 1}, .bitrate = 32'000, .expected_loss = 10});
    run_audio("music", {.format = {48'000, 2}, .content = AudioContent::Music, .bitrate = 128'000});
    return 0;
}
