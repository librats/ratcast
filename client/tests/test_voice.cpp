// VoiceChannel: the microphone silenced when muted, who is speaking and who
// is heard held for a moment, and two sides of a call talking — each hearing
// the other's voice, and no longer once it is muted.

#include "ratcast/client/voice.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "ratcast/engine/tone_source.h"
#include "ratcast/engine/virtual_output.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

/// A microphone the test speaks into by hand.
class HandMicrophone final : public AudioSource {
public:
    explicit HandMicrophone(AudioFormat format) : format_(format) {}
    const AudioFormat& format() const noexcept override { return format_; }
    Result<void> start(Sink sink, EndHandler) override {
        sink_ = std::move(sink);
        return {};
    }
    void stop() override { sink_ = {}; }

    /// 10 ms at `level`, alternating in sign: not a level the high-pass
    /// filter takes out.
    void say(float level, Timestamp captured) {
        MutableAudioFrame block = MutableAudioFrame::allocate(format_, format_.sample_rate / 100);
        const std::span<float> samples = block.samples();
        for (size_t i = 0; i < samples.size(); ++i) samples[i] = i % 2 ? level : -level;
        sink_(std::move(block).freeze(captured));
    }

private:
    const AudioFormat format_;
    Sink sink_;
};

/// A speaker the test pulls by hand.
class HandSpeaker final : public AudioOutput {
public:
    const AudioFormat& format() const noexcept override { return format_; }
    Result<void> start(Pull pull, EndHandler) override {
        pull_ = std::move(pull);
        return {};
    }
    void stop() override { pull_ = {}; }
    void play(Timestamp when) {
        std::vector<float> out(960);
        pull_(out, when);
    }

private:
    const AudioFormat format_{48'000, 2};
    Pull pull_;
};

TEST(VoiceChannel, MutedTheMicrophoneSendsSilence) {
    VoiceChannel voice(
        {.processing = {
             .echo_cancellation = false, .noise_suppression = false, .gain_control = false}});
    auto device = std::make_unique<HandMicrophone>(AudioFormat{48'000, 1});
    HandMicrophone& hand = *device;
    std::unique_ptr<AudioSource> microphone = voice.microphone(std::move(device));
    std::vector<float> peaks;
    ASSERT_TRUE(microphone->start(
        [&](AudioFrame block) {
            float peak = 0;
            for (float s : block.samples()) peak = std::max(peak, std::abs(s));
            peaks.push_back(peak);
        },
        {}));
    Timestamp at{1s};
    for (int i = 0; i < 20; ++i, at += 10ms) hand.say(0.3f, at);
    voice.set_muted(true);
    EXPECT_TRUE(voice.muted());
    const size_t before = peaks.size();
    for (int i = 0; i < 20; ++i, at += 10ms) hand.say(0.3f, at);
    ASSERT_GT(before, 10u);
    EXPECT_GT(peaks[before - 1], 0.1f) << "sound while not muted";
    ASSERT_GT(peaks.size(), before + 10);
    EXPECT_EQ(peaks.back(), 0.0f) << "nothing once muted";
}

TEST(VoiceChannel, SpeakingAndHearingHoldAMoment) {
    // Devices of another rate than configured: no processor, so speaking is
    // told by the level alone.
    ManualClock clock{Timestamp{10s}};
    VoiceChannel voice({.microphone = {16'000, 1}}, clock);
    auto device = std::make_unique<HandMicrophone>(AudioFormat{8'000, 1});
    HandMicrophone& hand = *device;
    std::unique_ptr<AudioSource> microphone = voice.microphone(std::move(device));
    ASSERT_TRUE(microphone->start([](AudioFrame) {}, {}));
    EXPECT_FALSE(voice.speaking());
    hand.say(0.3f, clock.now());
    EXPECT_TRUE(voice.speaking());
    clock.advance(200ms);
    hand.say(0.0f, clock.now());
    EXPECT_TRUE(voice.speaking()) << "held over a pause";
    clock.advance(200ms);
    EXPECT_FALSE(voice.speaking());
    hand.say(0.3f, clock.now());
    voice.set_muted(true);
    EXPECT_FALSE(voice.speaking()) << "not heard by anyone, muted";

    auto speaker_device = std::make_unique<HandSpeaker>();
    HandSpeaker& speaker = *speaker_device;
    std::unique_ptr<AudioOutput> output = voice.speaker(std::move(speaker_device));
    float level = 0.3f;
    ASSERT_TRUE(
        output->start([&](std::span<float> out, Timestamp) { std::ranges::fill(out, level); }, {}));
    speaker.play(clock.now());
    EXPECT_TRUE(voice.hearing());
    level = 0;
    clock.advance(400ms);
    speaker.play(clock.now());
    EXPECT_FALSE(voice.hearing());
}

/// One side: its voice, a speaker that plays to nobody, its call.
struct Side {
    explicit Side(float pitch) {
        speaker = voice.speaker(std::make_unique<VirtualOutput>(VirtualOutputConfig{}));
        microphone = voice.microphone(
            std::make_unique<ToneSource>(ToneConfig{.frequency = pitch, .level = 0.3f}));
    }

    // Tones on both sides: neither the echo canceller nor the noise
    // suppressor is for them (the engine's tests are).
    VoiceChannel voice{{.processing = {.echo_cancellation = false, .noise_suppression = false}}};
    std::unique_ptr<AudioOutput> speaker;
    std::unique_ptr<AudioSource> microphone;
    std::unique_ptr<Call> call;  ///< last: it plays into the speaker
};

CallConfig config(std::optional<RatsUrl> dial = std::nullopt) {
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    return CallConfig{
        .bind_address = "127.0.0.1",
        .dial = std::move(dial),
        .protocol = std::string("ratcast-test/") + test->test_suite_name() + "." + test->name(),
        .watch_network = false,
    };
}

TEST(VoiceChannel, TwoSidesTalkAndAMutedOneIsNotHeard) {
    if (!open_audio_encoder({})) GTEST_SKIP() << "no Opus here";
    Side host(440);
    Side guest(660);
    CallConfig host_config = config();
    host_config.playback.sound = host.speaker.get();
    host.call = std::move(*Call::start(host_config));
    CallConfig guest_config =
        config(RatsUrl{librats::HostEndpoint{"127.0.0.1", host.call->port()}});
    guest_config.playback.sound = guest.speaker.get();
    guest.call = std::move(*Call::start(guest_config));
    ASSERT_TRUE(publish_voice(*host.call, std::move(host.microphone)));
    ASSERT_TRUE(publish_voice(*guest.call, std::move(guest.microphone)));

    ASSERT_TRUE(wait_for([&] { return host.voice.hearing() && guest.voice.hearing(); }));
    EXPECT_TRUE(host.call->voice_stats());
    EXPECT_FALSE(host.call->sound_stats()) << "a voice, not the sound with the pictures";

    guest.voice.set_muted(true);
    EXPECT_TRUE(wait_for([&] { return !host.voice.hearing(); })) << "the guest muted";
    EXPECT_TRUE(guest.voice.hearing()) << "the host still talking";
    guest.voice.set_muted(false);
    EXPECT_TRUE(wait_for([&] { return host.voice.hearing(); }));
}

}  // namespace
