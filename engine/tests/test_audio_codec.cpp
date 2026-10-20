// Opus through the engine: blocks of any length in, frames numbered and timed
// the way MediaSession and a jitter buffer expect out, and the sound back —
// including what a loss or DTX's silence left out. Skipped in a build without
// libopus.

#include "ratcast/engine/audio_codec.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr AudioFormat kMono{48'000, 1};
constexpr size_t kFrameLength = 960;  // 20 ms at 48 kHz
const Timestamp kStart{1s};

/// `length` samples per channel of a 440 Hz tone at half scale, picking up at
/// sample `from` so that consecutive blocks join without a click.
AudioFrame tone(AudioFormat format, size_t from, size_t length, Timestamp capture_time,
                float amplitude = 0.5f) {
    MutableAudioFrame frame = MutableAudioFrame::allocate(format, length);
    std::span<float> out = frame.samples();
    for (size_t i = 0; i < length; ++i) {
        const double t = double(from + i) / format.sample_rate;
        const float v = amplitude * float(std::sin(2 * std::numbers::pi * 440 * t));
        for (size_t c = 0; c < format.channels; ++c) out[i * format.channels + c] = v;
    }
    return std::move(frame).freeze(capture_time);
}

AudioFrame silence(AudioFormat format, size_t length, Timestamp capture_time) {
    return tone(format, 0, length, capture_time, 0.0f);
}

float rms(std::span<const float> samples) {
    double sum = 0;
    for (float s : samples) sum += double(s) * s;
    return samples.empty() ? 0.0f : float(std::sqrt(sum / samples.size()));
}

EncodedFrame received(EncodedAudio& audio) {
    return {audio.info, Buffer::copy_of(audio.payload.bytes())};
}

class AudioCodecTest : public ::testing::Test {
protected:
    void open(AudioEncoderConfig config) {
        Result<std::unique_ptr<AudioEncoder>> encoder = open_audio_encoder(config);
        if (!encoder) GTEST_SKIP() << "no Opus here: " << to_string(encoder.error());
        encoder_ = std::move(*encoder);
        Result<std::unique_ptr<AudioDecoder>> decoder =
            open_audio_decoder({config.codec, config.format, {}});
        ASSERT_TRUE(decoder) << to_string(decoder.error());
        decoder_ = std::move(*decoder);
    }

    std::vector<EncodedAudio> encode(const AudioFrame& samples) {
        std::vector<EncodedAudio> out;
        const Result<void> encoded =
            encoder_->encode(samples, [&](EncodedAudio&& a) { out.push_back(std::move(a)); });
        EXPECT_TRUE(encoded) << to_string(encoded.error());
        return out;
    }

    /// A second of tone in 20 ms blocks.
    std::vector<EncodedAudio> encode_second() {
        std::vector<EncodedAudio> frames;
        for (size_t i = 0; i < 50; ++i) {
            for (EncodedAudio& a :
                 encode(tone(kMono, i * kFrameLength, kFrameLength, kStart + i * 20ms)))
                frames.push_back(std::move(a));
        }
        return frames;
    }

    std::unique_ptr<AudioEncoder> encoder_;
    std::unique_ptr<AudioDecoder> decoder_;
};

TEST(AudioFrameTest, HoldsInterleavedSamplesAndWhenTheFirstWasCaptured) {
    MutableAudioFrame mutable_frame = MutableAudioFrame::allocate({48'000, 2}, 480);
    ASSERT_EQ(mutable_frame.samples().size(), 960u);
    for (size_t i = 0; i < 960; ++i) mutable_frame.samples()[i] = float(i);
    const AudioFrame frame = std::move(mutable_frame).freeze(kStart);

    EXPECT_EQ(frame.length(), 480u);
    EXPECT_EQ(frame.duration(), 10ms);
    EXPECT_EQ(frame.capture_time(), kStart);
    EXPECT_EQ(frame.samples()[959], 959.0f);
    const AudioFrame copy = frame;  // shares the samples
    EXPECT_EQ(copy.samples().data(), frame.samples().data());
    EXPECT_TRUE(AudioFrame{}.empty());
}

TEST(AudioCodecConfigTest, RefusesWhatOpusDoesNotTake) {
    const auto refused = [](auto change) {
        AudioEncoderConfig config;
        change(config);
        const Result<std::unique_ptr<AudioEncoder>> encoder = open_audio_encoder(config);
        return !encoder && encoder.error() == Errc::InvalidArgument;
    };
    EXPECT_TRUE(refused([](auto& c) { c.format.sample_rate = 44'100; }));
    EXPECT_TRUE(refused([](auto& c) { c.format.channels = 3; }));
    EXPECT_TRUE(refused([](auto& c) { c.frame_duration = 15ms; }));
    EXPECT_TRUE(refused([](auto& c) { c.bitrate = 1'000; }));
    EXPECT_TRUE(refused([](auto& c) { c.codec = Codec::AV1; }));
    EXPECT_TRUE(refused([](auto& c) { c.expected_loss = 101; }));

    const Result<std::unique_ptr<AudioDecoder>> decoder =
        open_audio_decoder({Codec::Opus, {44'100, 2}, {}});
    ASSERT_FALSE(decoder);
    EXPECT_EQ(decoder.error(), Errc::InvalidArgument);
}

TEST_F(AudioCodecTest, CutsBlocksOfAnyLengthIntoFramesTimedByTheirFirstSample) {
    open({});
    // A second in uneven blocks, as a capture device's quantum would give it.
    std::vector<EncodedAudio> frames;
    size_t at = 0;
    for (size_t length : {100, 1000, 333, 4567, 960, 7, 41'033}) {
        for (EncodedAudio& a : encode(tone(kMono, at, length, kStart + audio_duration(at, 48'000))))
            frames.push_back(std::move(a));
        at += length;
    }
    ASSERT_EQ(at, 48'000u);
    ASSERT_EQ(frames.size(), 50u);
    for (size_t i = 0; i < frames.size(); ++i) {
        const FrameInfo& info = frames[i].info;
        EXPECT_EQ(info.number, i);
        // Each block's time is rounded to the microsecond, and so is a frame's
        // offset into it.
        EXPECT_LE(std::chrono::abs(info.capture_time - kStart - 20ms * int(i)), 1us);
        EXPECT_TRUE(info.keyframe);
        EXPECT_TRUE(info.references.empty());
        EXPECT_GT(frames[i].payload.size(), 0u);
    }
}

TEST_F(AudioCodecTest, RefusesSamplesOfAnotherFormat) {
    open({});
    const Result<void> encoded =
        encoder_->encode(tone({48'000, 2}, 0, 960, kStart), [](EncodedAudio&&) {});
    ASSERT_FALSE(encoded);
    EXPECT_EQ(encoded.error(), Errc::InvalidArgument);
}

TEST_F(AudioCodecTest, DecodesWhatItEncoded) {
    open({.format = kMono, .content = AudioContent::Music, .bitrate = 64'000});
    std::vector<float> out;
    for (EncodedAudio& a : encode_second()) {
        Result<AudioFrame> decoded = decoder_->decode(received(a));
        ASSERT_TRUE(decoded) << to_string(decoded.error());
        EXPECT_EQ(decoded->length(), kFrameLength);
        EXPECT_EQ(decoded->capture_time(), a.info.capture_time);
        out.insert(out.end(), decoded->samples().begin(), decoded->samples().end());
    }
    // Past the codec's start-up, the tone at the level it went in.
    const float level = rms(std::span(out).subspan(out.size() / 2));
    EXPECT_NEAR(level, 0.5f / std::numbers::sqrt2_v<float>, 0.05f);
    EXPECT_EQ(decoder_->stats().decoded, 1s);
}

TEST_F(AudioCodecTest, RefusesAFrameThatIsNotOpus) {
    open({});
    const EncodedFrame empty{{}, Buffer{}};
    const Result<AudioFrame> decoded = decoder_->decode(empty);
    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error(), Errc::Malformed);

    // Nor does a gap before one hold FEC: it is concealed.
    Result<AudioFrame> filler = decoder_->conceal(20ms, kStart, &empty);
    ASSERT_TRUE(filler);
    EXPECT_EQ(filler->duration(), 20ms);
    EXPECT_EQ(decoder_->stats().concealed, 20ms);
}

TEST_F(AudioCodecTest, SendsAlmostNothingInSilenceWithDtx) {
    open({.dtx = true});
    std::vector<EncodedAudio> frames = encode_second();
    const size_t sounding = frames.size();
    for (size_t i = 0; i < 100; ++i) {  // two seconds of silence
        for (EncodedAudio& a : encode(silence(kMono, kFrameLength, kStart + 1s + i * 20ms)))
            frames.push_back(std::move(a));
    }
    // A few frames as the sound ends, then one every 400 ms for comfort noise.
    EXPECT_LT(frames.size() - sounding, 20u);
    for (size_t i = 1; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i].info.number, frames[i - 1].info.number + 1);
        EXPECT_GT(frames[i].info.capture_time, frames[i - 1].info.capture_time);
    }

    // The gap in time, with none in numbers, is silence: the decoder fills it
    // with comfort noise, as quiet as what went in.
    Timestamp expected = kStart;
    for (EncodedAudio& a : frames) {
        if (a.info.capture_time > expected) {
            Result<AudioFrame> filler = decoder_->conceal(a.info.capture_time - expected, expected);
            ASSERT_TRUE(filler);
            EXPECT_LT(rms(filler->samples()), 0.01f);
        }
        Result<AudioFrame> decoded = decoder_->decode(received(a));
        ASSERT_TRUE(decoded);
        expected = a.info.capture_time + decoded->duration();
    }
    EXPECT_EQ(decoder_->stats().decoded + decoder_->stats().concealed, expected - kStart);
}

TEST_F(AudioCodecTest, ConcealsALossInWhole2_5Milliseconds) {
    open({});
    for (EncodedAudio& a : encode_second()) ASSERT_TRUE(decoder_->decode(received(a)));

    Result<AudioFrame> filler = decoder_->conceal(26ms, kStart + 1s);
    ASSERT_TRUE(filler);
    EXPECT_EQ(filler->duration(), 25ms);
    EXPECT_EQ(filler->capture_time(), kStart + 1s);
    EXPECT_GT(rms(filler->samples()), 0.01f);  // the tone goes on, fading
    EXPECT_EQ(decoder_->stats().concealed, 25ms);

    Result<AudioFrame> nothing = decoder_->conceal(2ms, kStart + 1s + 25ms);
    ASSERT_TRUE(nothing);
    EXPECT_TRUE(nothing->empty());
}

TEST_F(AudioCodecTest, RecoversALostFrameFromTheFecOfTheNext) {
    open({.expected_loss = 10});
    std::vector<EncodedAudio> frames = encode_second();
    for (size_t i = 0; i < 25; ++i) ASSERT_TRUE(decoder_->decode(received(frames[i])));

    // Frame 25 lost: its sound comes from 26's FEC.
    const EncodedFrame next = received(frames[26]);
    Result<AudioFrame> filler = decoder_->conceal(20ms, frames[25].info.capture_time, &next);
    ASSERT_TRUE(filler);
    EXPECT_EQ(filler->length(), kFrameLength);
    ASSERT_TRUE(decoder_->decode(next));
#ifdef RATCAST_OPUS_HAS_LBRR
    EXPECT_EQ(decoder_->stats().recovered, 20ms);
    EXPECT_EQ(decoder_->stats().concealed, 0ms);
#endif
    // The tone, not a fade.
    EXPECT_NEAR(rms(filler->samples()), 0.5f / std::numbers::sqrt2_v<float>, 0.05f);
}

TEST_F(AudioCodecTest, ConcealsAllButTheLastOfSeveralLostFrames) {
    open({.expected_loss = 10});
    std::vector<EncodedAudio> frames = encode_second();
    for (size_t i = 0; i < 25; ++i) ASSERT_TRUE(decoder_->decode(received(frames[i])));

    // Frames 25 and 26 lost: 27's FEC holds only 26.
    const EncodedFrame next = received(frames[27]);
    Result<AudioFrame> filler = decoder_->conceal(40ms, frames[25].info.capture_time, &next);
    ASSERT_TRUE(filler);
    EXPECT_EQ(filler->length(), 2 * kFrameLength);
#ifdef RATCAST_OPUS_HAS_LBRR
    EXPECT_EQ(decoder_->stats().recovered, 20ms);
    EXPECT_EQ(decoder_->stats().concealed, 20ms);
#endif
}

TEST_F(AudioCodecTest, FramesGrowWithTheBitrate) {
    open({.content = AudioContent::Music, .bitrate = 16'000});
    const auto average = [](const std::vector<EncodedAudio>& frames) {
        size_t bytes = 0;
        for (size_t i = 10; i < frames.size(); ++i) bytes += frames[i].payload.size();
        return double(bytes) / double(frames.size() - 10);
    };
    const double low = average(encode_second());
    encoder_->set_bitrate(64'000);
    const double high = average(encode_second());
    EXPECT_GT(high, 2.5 * low);
    EXPECT_NEAR(high, 64'000 / 8 / 50.0, 40.0);
}

}  // namespace
