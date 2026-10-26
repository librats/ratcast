// SwitchableOutput over outputs the test pulls by hand: what is played goes
// on to the device switched to, the one before stopped; a switch while
// stopped waits for the start; a device of another format, or one that will
// not start, leaves the one before playing; and the device's end is heard.

#include "ratcast/engine/switchable_output.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "hand_output.h"

using namespace ratcast;
using ratcast::test::HandOutput;

namespace {

/// `device`, lent: the test keeps it, to pull and look at after the switch
/// has let go of it.
class Lent final : public AudioOutput {
public:
    explicit Lent(HandOutput& device) : device_(device) {}
    const AudioFormat& format() const noexcept override { return device_.format(); }
    Result<void> start(Pull pull, EndHandler ended) override {
        return device_.start(std::move(pull), std::move(ended));
    }
    void stop() override { device_.stop(); }

private:
    HandOutput& device_;
};

std::unique_ptr<AudioOutput> lend(HandOutput& device) { return std::make_unique<Lent>(device); }

AudioOutput::Pull constant(float value) {
    return [value](std::span<float> out, Timestamp) { std::ranges::fill(out, value); };
}

bool all(const std::vector<float>& samples, float value) {
    return !samples.empty() && std::ranges::all_of(samples, [&](float s) { return s == value; });
}

}  // namespace

TEST(SwitchableOutput, WhatPlaysGoesOnToTheDeviceSwitchedTo) {
    HandOutput headset;
    HandOutput speakers;
    SwitchableOutput output(lend(headset));
    EXPECT_EQ(output.format(), headset.format());

    ASSERT_TRUE(output.start(constant(0.5f), {}));
    EXPECT_TRUE(all(headset.pull(), 0.5f));

    ASSERT_TRUE(output.switch_to(lend(speakers)));
    EXPECT_FALSE(headset.running()) << "one device pulls at a time";
    EXPECT_TRUE(all(speakers.pull(), 0.5f));

    output.stop();
    EXPECT_FALSE(speakers.running());
}

TEST(SwitchableOutput, ASwitchWhileStoppedWaitsForTheStart) {
    HandOutput headset;
    HandOutput speakers;
    SwitchableOutput output(lend(headset));
    ASSERT_TRUE(output.switch_to(lend(speakers)));
    EXPECT_FALSE(speakers.running());

    ASSERT_TRUE(output.start(constant(0.25f), {}));
    EXPECT_EQ(headset.starts, 0);
    EXPECT_TRUE(all(speakers.pull(), 0.25f));
    EXPECT_FALSE(output.start(constant(0.25f), {})) << "started already";
    output.stop();
    ASSERT_TRUE(output.start(constant(0.25f), {})) << "and again";
    EXPECT_EQ(speakers.starts, 2);
}

TEST(SwitchableOutput, ADeviceThatWillNotDoLeavesTheOneBeforePlaying) {
    HandOutput headset;
    HandOutput mono(AudioFormat{48'000, 1});
    HandOutput gone;
    gone.refuse = true;
    SwitchableOutput output(lend(headset));
    ASSERT_TRUE(output.start(constant(0.5f), {}));

    EXPECT_EQ(output.switch_to(lend(mono)).error(), Errc::InvalidArgument);
    EXPECT_EQ(mono.starts, 0);
    EXPECT_EQ(output.switch_to(lend(gone)).error(), Errc::Unavailable);
    EXPECT_EQ(output.switch_to(nullptr).error(), Errc::InvalidArgument);
    EXPECT_TRUE(all(headset.pull(), 0.5f)) << "still playing where it was";
}

TEST(SwitchableOutput, TheDevicesEndIsHeardAndASwitchPlaysOn) {
    HandOutput headset;
    HandOutput speakers;
    SwitchableOutput output(lend(headset));
    int ends = 0;
    ASSERT_TRUE(output.start(constant(0.5f), [&](Result<void>) { ++ends; }));
    headset.end();  // unplugged
    EXPECT_EQ(ends, 1);

    ASSERT_TRUE(output.switch_to(lend(speakers)));
    EXPECT_TRUE(all(speakers.pull(), 0.5f));
    speakers.end();
    EXPECT_EQ(ends, 2) << "the same handler, on the new device";
}
