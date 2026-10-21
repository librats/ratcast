// The AudioProcessor on WebRTC's AudioProcessing, in virtual time: blocks
// of any length come back in 10 ms with their capture times, the speaker's
// echo is taken out of the microphone, steady noise is suppressed, and a
// voice is told from silence.

#include "ratcast/engine/audio_processing.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kRate = 48'000;
constexpr size_t kChunk = 480;  ///< 10 ms
const AudioFormat kMono{kRate, 1};
const AudioFormat kStereo{kRate, 2};

AudioFrame frame(const AudioFormat& format, std::span<const float> samples, Timestamp captured) {
    MutableAudioFrame out = MutableAudioFrame::allocate(format, samples.size() / format.channels);
    std::ranges::copy(samples, out.samples().begin());
    return std::move(out).freeze(captured);
}

double energy(std::span<const float> samples) {
    double sum = 0;
    for (float s : samples) sum += double(s) * s;
    return sum;
}

std::vector<float> noise(size_t length, float level, uint32_t seed) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> unit(-level, level);
    std::vector<float> samples(length);
    for (float& s : samples) s = unit(random);
    return samples;
}

/// A vowel, more or less: harmonics of 140 Hz shaped by two formants,
/// swelling four times a second as speech does.
std::vector<float> vowel(size_t length) {
    std::vector<float> samples(length);
    for (size_t n = 0; n < length; ++n) {
        const double t = double(n) / kRate;
        double value = 0;
        for (int h = 1; h <= 30; ++h) {
            const double f = 140.0 * h;
            const double formants = std::exp(-std::pow((f - 700) / 200, 2)) +
                                    0.6 * std::exp(-std::pow((f - 1200) / 250, 2)) + 0.05;
            value += formants * std::sin(2 * std::numbers::pi * f * t);
        }
        const double swell = 0.6 + 0.4 * std::sin(2 * std::numbers::pi * 4 * t);
        samples[n] = float(0.08 * swell * value);
    }
    return samples;
}

/// A call in virtual time, 10 ms a step: the speaker is handed `far` 20 ms
/// before it is heard; the microphone hears `near`, and the far side's echo
/// 10 ms after it is played at `echo` of its level, and is processed 10 ms
/// after capture. What came out of the processor, and what went in.
struct Heard {
    std::vector<float> in;
    std::vector<float> out;
};

Heard run(AudioProcessor& processor, ManualClock& clock, const std::vector<float>& far,
          const std::vector<float>& near, float echo) {
    const size_t steps = near.size() / kChunk;
    Heard result;
    std::vector<float> speaker(2 * kChunk);
    std::vector<float> microphone(kChunk);
    const Timestamp start = clock.now();
    for (size_t k = 0; k < steps; ++k) {
        const Timestamp now = start + Duration(10ms) * int64_t(k);
        clock.set(now);
        // Heard from now + 20 ms on.
        for (size_t i = 0; i < kChunk; ++i) {
            const size_t n = (k + 2) * kChunk + i;
            speaker[2 * i] = speaker[2 * i + 1] = n < far.size() ? far[n] : 0.0f;
        }
        processor.played(speaker, now + 20ms);
        if (k == 0) continue;
        // Captured over the last 10 ms.
        for (size_t i = 0; i < kChunk; ++i) {
            const size_t n = (k - 1) * kChunk + i;
            const float heard = n >= kChunk && n - kChunk < far.size() ? far[n - kChunk] : 0.0f;
            microphone[i] = near[n] + echo * heard;
        }
        result.in.insert(result.in.end(), microphone.begin(), microphone.end());
        const std::optional<AudioFrame> cleaned =
            processor.process(frame(kMono, microphone, now - 10ms));
        if (cleaned) {
            result.out.insert(result.out.end(), cleaned->samples().begin(),
                              cleaned->samples().end());
        }
    }
    return result;
}

/// Of the last `seconds` of a run: energy out over energy in.
double kept(const Heard& run, double seconds) {
    const size_t tail = size_t(seconds * kRate);
    return energy(std::span(run.out).last(tail)) / energy(std::span(run.in).last(tail));
}

class AudioProcessingTest : public ::testing::Test {
protected:
    std::unique_ptr<AudioProcessor> open(AudioProcessingConfig config = {}) {
        config.capture = kMono;
        config.playback = kStereo;
        Result<std::unique_ptr<AudioProcessor>> processor = open_audio_processor(config, clock_);
        if (!processor) return nullptr;
        return std::move(*processor);
    }

    void SetUp() override {
        if (!open()) GTEST_SKIP() << "built without WebRTC AudioProcessing";
    }

    ManualClock clock_{Timestamp{100s}};
};

TEST_F(AudioProcessingTest, BlocksOfAnyLengthComeBackInTensOfMilliseconds) {
    auto processor = open();
    // 7 ms blocks: every 10 ms complete comes back, stamped as captured.
    constexpr size_t kBlock = 336;
    const Timestamp start{100s};
    std::vector<float> silence(kBlock, 0.0f);
    size_t out = 0;
    std::optional<Timestamp> next;
    for (size_t b = 0; b < 100; ++b) {
        const Timestamp captured = start + audio_duration(b * kBlock, kRate);
        clock_.set(captured + 7ms);
        const std::optional<AudioFrame> cleaned =
            processor->process(frame(kMono, silence, captured));
        if (!cleaned) continue;
        EXPECT_EQ(cleaned->length() % kChunk, 0u);
        EXPECT_EQ(cleaned->capture_time(), next.value_or(start));
        next = cleaned->capture_time() + cleaned->duration();
        out += cleaned->length();
    }
    EXPECT_EQ(out, 100 * kBlock / kChunk * kChunk) << "all but the last part-chunk";
}

TEST_F(AudioProcessingTest, TakesTheSpeakersEchoOut) {
    // Eight seconds of the far side talking (noise, which tells the canceller
    // the most), half of it coming back; the last two judged, against the
    // same with nothing on but the high-pass filter.
    const size_t length = 8 * kRate;
    const std::vector<float> far = noise(length, 0.3f, 1);
    const std::vector<float> quiet(length, 0.0f);
    auto cancelling = open();
    const double with = kept(run(*cancelling, clock_, far, quiet, 0.5f), 2);
    auto plain =
        open({.echo_cancellation = false, .noise_suppression = false, .gain_control = false});
    const double without = kept(run(*plain, clock_, far, quiet, 0.5f), 2);
    EXPECT_LT(with, 0.01) << "at least 20 dB of the echo taken out";
    EXPECT_GT(without, 0.5) << "the echo, as it came";
}

TEST_F(AudioProcessingTest, SuppressesSteadyNoise) {
    auto processor = open({.echo_cancellation = false, .gain_control = false});
    const size_t length = 4 * kRate;
    const Heard hissing = run(*processor, clock_, {}, noise(length, 0.05f, 2), 0.0f);
    EXPECT_LT(kept(hissing, 2), 0.1) << "at least 10 dB of the hiss taken out";
}

TEST_F(AudioProcessingTest, TellsAVoiceFromSilence) {
    auto processor = open({.echo_cancellation = false});
    run(*processor, clock_, {}, std::vector<float>(kRate, 0.0f), 0.0f);
    EXPECT_FALSE(processor->voice());

    // A vowel, every 10 ms of it asked about.
    const std::vector<float> spoken = vowel(2 * kRate);
    size_t voiced = 0;
    for (size_t k = 0; k < spoken.size() / kChunk; ++k) {
        clock_.advance(10ms);
        processor->process(
            frame(kMono, std::span(spoken).subspan(k * kChunk, kChunk), clock_.now() - 10ms));
        voiced += processor->voice() ? 1 : 0;
    }
    EXPECT_GT(voiced, spoken.size() / kChunk / 2) << "most of it heard as a voice";
}

TEST_F(AudioProcessingTest, RefusesFormatsItCannotTake) {
    for (AudioFormat format : {AudioFormat{44'100 + 1, 1}, AudioFormat{96'000, 1},
                               AudioFormat{48'000, 3}, AudioFormat{48'000, 0}}) {
        Result<std::unique_ptr<AudioProcessor>> processor =
            open_audio_processor({.capture = format}, clock_);
        ASSERT_FALSE(processor);
        EXPECT_EQ(processor.error(), Errc::InvalidArgument);
    }
}

}  // namespace
