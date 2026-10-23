#include "ratcast/client/voice_meter.h"

#include <gtest/gtest.h>

#include <string>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

PeerId peer(char n) { return PeerId::from_hex(std::string(64, n)).value(); }

}  // namespace

TEST(VoiceMeter, MeasuresEachVoiceFromTheStartHoweverLateItCame) {
    ManualClock clock;
    VoiceMeter meter(clock, 100ms);
    meter.start();

    clock.advance(300ms);
    meter.heard(peer('1'), 40ms);
    clock.advance(500ms);  // the second joins later
    meter.heard(peer('2'), 60ms);
    meter.heard(peer('1'), 50ms);  // a break of 500 ms in the first
    clock.advance(20ms);
    meter.heard(peer('2'), 80ms);

    const std::map<PeerId, PlaybackStats> voices = meter.voices();
    ASSERT_EQ(voices.size(), 2u);
    const PlaybackStats& first = voices.at(peer('1'));
    EXPECT_EQ(first.first_picture, 300ms);
    EXPECT_EQ(first.pictures, 2u);
    EXPECT_EQ(first.stalls, 1u);
    EXPECT_EQ(first.stalled, 500ms);
    const PlaybackStats& second = voices.at(peer('2'));
    EXPECT_EQ(second.first_picture, 800ms) << "counted from start(), not its first frame";
    EXPECT_EQ(second.stalls, 0u);
    EXPECT_EQ(second.latency.mean(), 70ms);

    const PlaybackStats all = VoiceMeter::together(voices);
    EXPECT_EQ(all.pictures, 4u);
    EXPECT_EQ(all.first_picture, 800ms) << "when the last voice was first heard";
    EXPECT_EQ(all.stalls, 1u);
    EXPECT_EQ(all.longest_gap, 500ms);
    EXPECT_EQ(all.latency.count(), 4u);
    EXPECT_EQ(all.latency.mean(), 57'500us);
    EXPECT_EQ(all.latency.max(), 80ms);
}

TEST(VoiceMeter, IntervalsAndStartingOver) {
    ManualClock clock;
    VoiceMeter meter(clock, 100ms);
    meter.start();
    meter.heard(peer('1'));
    meter.heard(peer('2'));
    EXPECT_EQ(meter.take_intervals().at(peer('1')).pictures, 1u);
    clock.advance(20ms);
    meter.heard(peer('1'));
    const std::map<PeerId, PlaybackStats> interval = meter.take_intervals();
    EXPECT_EQ(interval.at(peer('1')).pictures, 1u);
    EXPECT_EQ(interval.at(peer('2')).pictures, 0u) << "silent this interval";
    EXPECT_FALSE(VoiceMeter::together(interval).first_picture) << "a total's";

    meter.start();
    EXPECT_TRUE(meter.voices().empty());
    EXPECT_FALSE(VoiceMeter::together({}).first_picture);
}
