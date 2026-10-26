// Sound devices the user chooses, against stand-ins on the user's PipeWire
// daemon: listed with the system's own by a name that lasts, a microphone
// chosen heard rather than the default, speakers chosen played to and what
// they play heard, and one not there the default in its place. Skipped where there is no PipeWire
// (CI).

#include "ratcast/engine/audio_device.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "heard_sound.h"
#include "ratcast/engine/tone_source.h"
#include "test_audio_node.h"

using namespace ratcast;
using namespace ratcast::test;
using namespace std::chrono_literals;

namespace {

constexpr AudioFormat kMono{48'000, 1};

/// A name of this process's own, so that tests side by side tell theirs apart.
std::string device_name(const std::string& what) {
    return "ratcast-test-" + what + "-" + std::to_string(getpid());
}

bool listed(const Result<std::vector<AudioDeviceInfo>>& devices, const std::string& id) {
    return devices && std::ranges::any_of(*devices, [&](const AudioDeviceInfo& device) {
               return device.id == id && device.name == id;  // its description, here its name
           });
}

}  // namespace

TEST(AudioDevices, AreListedWithTheSystemsOwn) {
    const std::string microphone_name = device_name("microphone");
    const std::string speakers_name = device_name("speakers");
    auto microphone = TestAudioNode::create(TestAudioNode::Kind::Source, microphone_name);
    if (!microphone) GTEST_SKIP() << "no PipeWire daemon here";
    auto speakers = TestAudioNode::create(TestAudioNode::Kind::Sink, speakers_name);
    ASSERT_TRUE(speakers);

    const Result<std::vector<AudioDeviceInfo>> inputs = list_audio_inputs();
    const Result<std::vector<AudioDeviceInfo>> outputs = list_audio_outputs();
    ASSERT_TRUE(inputs);
    ASSERT_TRUE(outputs);
    EXPECT_TRUE(listed(inputs, microphone_name));
    EXPECT_FALSE(listed(inputs, speakers_name));
    EXPECT_TRUE(listed(outputs, speakers_name));
    EXPECT_FALSE(listed(outputs, microphone_name));
}

TEST(AudioDevices, AMicrophoneChosenIsHeard) {
    const std::string name = device_name("chosen-microphone");
    auto microphone = TestAudioNode::create(TestAudioNode::Kind::Source, name);
    if (!microphone) GTEST_SKIP() << "no PipeWire daemon here";
    Result<std::unique_ptr<AudioSource>> capture =
        open_audio_capture({.format = kMono, .device = name});
    ASSERT_TRUE(capture);
    Heard heard;
    ASSERT_TRUE((*capture)->start(heard.sink(), heard.ended()));
    const std::vector<AudioFrame> tone = heard.wait_for_tone(200ms);
    ASSERT_FALSE(tone.empty()) << "the default heard instead";
    EXPECT_NEAR(rms(tone), TestAudioNode::kLevel / std::sqrt(2.0f), 0.02f);
}

TEST(AudioDevices, SpeakersChosenArePlayedTo) {
    const std::string name = device_name("chosen-speakers");
    auto speakers = TestAudioNode::create(TestAudioNode::Kind::Sink, name);
    if (!speakers) GTEST_SKIP() << "no PipeWire daemon here";
    Result<std::unique_ptr<AudioOutput>> output =
        open_audio_output({.format = kMono, .device = name});
    ASSERT_TRUE(output);
    const ToneConfig tone{kMono, 440.0f, 0.25f};
    uint64_t played = 0;  // on the output's thread
    ASSERT_TRUE((*output)->start(
        [&](std::span<float> out, Timestamp play_time) {
            const AudioFrame block = draw_tone(tone, played, out.size(), play_time);
            std::ranges::copy(block.samples(), out.begin());
            played += out.size();
        },
        {}));
    const auto deadline = std::chrono::steady_clock::now() + kSoundPatience;
    while (speakers->loudest() < 0.2f && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    (*output)->stop();
    EXPECT_NEAR(speakers->loudest(), 0.25f, 0.02f) << "played elsewhere";
}

TEST(AudioDevices, WhatSpeakersChosenPlayIsHeard) {
    const std::string name = device_name("echoing-speakers");
    auto speakers = TestAudioNode::create(TestAudioNode::Kind::Sink, name);
    if (!speakers) GTEST_SKIP() << "no PipeWire daemon here";
    Result<std::unique_ptr<AudioOutput>> output =
        open_audio_output({.format = kMono, .device = name});
    ASSERT_TRUE(output);
    const ToneConfig tone{kMono, 440.0f, 0.25f};
    uint64_t played = 0;  // on the output's thread
    ASSERT_TRUE((*output)->start(
        [&](std::span<float> out, Timestamp play_time) {
            const AudioFrame block = draw_tone(tone, played, out.size(), play_time);
            std::ranges::copy(block.samples(), out.begin());
            played += out.size();
        },
        {}));
    Result<std::unique_ptr<AudioSource>> capture = open_audio_capture(
        {.format = kMono, .source = AudioCaptureSource::SystemSound, .device = name});
    ASSERT_TRUE(capture);
    Heard heard;
    ASSERT_TRUE((*capture)->start(heard.sink(), heard.ended()));
    const std::vector<AudioFrame> heard_tone = heard.wait_for_tone(200ms);
    (*capture)->stop();
    (*output)->stop();
    ASSERT_FALSE(heard_tone.empty()) << "the default output heard instead";
    EXPECT_NEAR(rms(heard_tone), 0.25f / std::sqrt(2.0f), 0.02f);
}

TEST(AudioDevices, OneNotThereIsTheDefaultInItsPlace) {
    const Result<std::vector<AudioDeviceInfo>> inputs = list_audio_inputs();
    if (!inputs || inputs->empty()) GTEST_SKIP() << "no microphone here";
    Result<std::unique_ptr<AudioSource>> capture =
        open_audio_capture({.format = kMono, .device = device_name("no-such-microphone")});
    ASSERT_TRUE(capture);
    Heard heard;
    ASSERT_TRUE((*capture)->start(heard.sink(), heard.ended()));
    EXPECT_FALSE(heard.wait_for_more(200ms).empty());
    (*capture)->stop();
}
