// The call monitor's reports from made-up counts: rates over the interval,
// what counts started over, each peer's voice and camera, the quality of each
// part and of the call, the lines a console shows. Over a real call:
// test_call.cpp.

#include "ratcast/client/call_monitor.h"
#include "ratcast/client/cpu_meter.h"

#include <gtest/gtest.h>

#include <chrono>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

const PeerId kAnn = PeerId::from_hex(std::string(64, 'a')).value();
const PeerId kBob = PeerId::from_hex(std::string(64, 'b')).value();

/// Two snapshots a second apart.
struct Snapshots {
    CallSnapshot earlier{.at = Timestamp{} + 10s};
    CallSnapshot now{.at = Timestamp{} + 11s};
};

SentStreamStats screen(uint64_t pictures, uint64_t frames, uint64_t bytes,
                       Duration encode_time = {}) {
    VideoPublisherStats video;
    video.pictures = pictures;
    video.frames = frames;
    video.bytes = bytes;
    video.encode_time = encode_time;
    video.width = 1920;
    video.height = 1080;
    video.encoder = "libsvtav1";
    OutgoingStreamStats session;
    session.subscribers = 1;
    session.bitrate = 5'000'000;
    return {1, "screen", video, session};
}

ReceivedVideoStats pictures(uint64_t frames, Timestamp first, Duration step) {
    ReceivedVideoStats stats;
    stats.player.frames = frames;
    stats.player.pictures = frames;
    stats.player.decoder = "libdav1d";
    for (uint64_t i = 0; i < frames; ++i) stats.player.pace.picture(first + step * int64_t(i));
    stats.stream.frames.received = frames;
    return stats;
}

AudioPlayerStats sound(uint64_t frames, Duration concealed) {
    AudioPlayerStats stats;
    stats.frames = frames;
    stats.jitter.played = frames;
    stats.jitter.concealed = concealed;
    return stats;
}

librats::PathStats path(uint64_t delivered, uint64_t lost) {
    librats::PathStats stats;
    stats.valid = true;
    stats.srtt_us = 20'000;
    stats.bandwidth_bps = 10'000'000;
    stats.bytes_delivered = delivered;
    stats.bytes_lost = lost;
    return stats;
}

}  // namespace

TEST(CallMonitor, RatesAreOverTheInterval) {
    Snapshots s;
    s.earlier.sent = {screen(600, 600, 5'000'000, 3s)};
    s.now.sent = {screen(660, 660, 5'500'000, 3s + 60 * 5ms)};
    s.earlier.cpu = 2s;
    s.now.cpu = 2s + 500ms;

    const CallReport report = compare(s.earlier, s.now);
    EXPECT_EQ(report.interval, 1s);
    EXPECT_DOUBLE_EQ(report.cpu, 50);
    ASSERT_EQ(report.sent.size(), 1u);
    const SentStreamReport& sent = report.sent.front();
    EXPECT_EQ(sent.label, "screen");
    EXPECT_DOUBLE_EQ(sent.fps, 60);
    EXPECT_DOUBLE_EQ(sent.source_fps, 60);
    EXPECT_DOUBLE_EQ(sent.bitrate, 4'000'000);
    EXPECT_EQ(sent.target, 5'000'000u);
    EXPECT_EQ(sent.encode_time, 5ms);
    EXPECT_EQ(sent.viewers, 1u);
    EXPECT_EQ(sent.verdict.quality, Quality::Good);
    EXPECT_EQ(report.verdict.quality, Quality::Good);
}

TEST(CallMonitor, AnEncoderSlowerThanThePicturesIsPoor) {
    Snapshots s;
    s.earlier.sent = {screen(0, 0, 0)};
    s.now.sent = {screen(60, 60, 500'000, 60 * 12ms)};  // 12 ms of 16.7: near its limit
    EXPECT_EQ(compare(s.earlier, s.now).sent.front().verdict.quality, Quality::Fair);

    s.now.sent = {screen(60, 40, 500'000, 40 * 25ms)};
    std::get<VideoPublisherStats>(s.now.sent.front().publisher).skipped = 20;
    const CallReport report = compare(s.earlier, s.now);
    EXPECT_EQ(report.sent.front().verdict.quality, Quality::Poor);
    EXPECT_EQ(report.verdict.quality, Quality::Poor);
    EXPECT_EQ(report.verdict.reason.rfind("sent screen: ", 0), 0u) << report.verdict.reason;
}

TEST(CallMonitor, FramesDroppedForAFullQueueArePoor) {
    Snapshots s;
    s.earlier.sent = {screen(0, 0, 0)};
    s.now.sent = {screen(30, 30, 100'000)};
    s.now.sent.front().session.backpressure_drops = 3;
    const SentStreamReport sent = compare(s.earlier, s.now).sent.front();
    EXPECT_EQ(sent.dropped, 3u);
    EXPECT_EQ(sent.verdict.quality, Quality::Poor);
    EXPECT_EQ(sent.verdict.reason, "frames dropped: the path is full");
}

TEST(CallMonitor, PicturesPlayedShowTheirFrameTime) {
    Snapshots s;
    const Timestamp start = Timestamp{} + 9s;
    s.earlier.video = pictures(30, start, 33ms);
    s.now.video = pictures(60, start, 33ms);
    s.now.video->stream.bytes = 250'000;

    const CallReport report = compare(s.earlier, s.now);
    ASSERT_TRUE(report.video);
    EXPECT_DOUBLE_EQ(report.video->fps, 30);
    EXPECT_DOUBLE_EQ(report.video->bitrate, 2'000'000);
    EXPECT_EQ(report.video->pace.intervals().count(), 30u);
    EXPECT_GE(report.video->pace.intervals().percentile(0.5), 33ms);
    EXPECT_LE(report.video->pace.intervals().percentile(0.5), 34ms);
    EXPECT_EQ(report.video->pace.jitter(), Duration{});
    EXPECT_EQ(report.video->verdict.quality, Quality::Good);

    s.now.video->stream.frames.late = 2;
    EXPECT_EQ(compare(s.earlier, s.now).video->verdict.quality, Quality::Fair);
    s.now.video->stream.frames.lost = 1;
    EXPECT_EQ(compare(s.earlier, s.now).verdict.reason, "pictures: frames lost");
}

TEST(CallMonitor, PicturesSealedEndToEndSaySoAndThoseWithNoKeyArePoor) {
    Snapshots s;
    const Timestamp start = Timestamp{} + 9s;
    s.earlier.video = pictures(30, start, 33ms);
    s.now.video = pictures(60, start, 33ms);
    EXPECT_FALSE(compare(s.earlier, s.now).video->sealed);
    s.now.video->stream.opened = 30;
    CallReport report = compare(s.earlier, s.now);
    EXPECT_TRUE(report.video->sealed);
    EXPECT_NE(report_lines(report).front().find("sealed"), std::string::npos);
    EXPECT_EQ(report.verdict.quality, Quality::Good);

    s.now.video->stream.unopened = 2;
    report = compare(s.earlier, s.now);
    EXPECT_EQ(report.video->unopened, 2u);
    EXPECT_EQ(report.verdict.reason, "pictures: frames it has no key for");
}

TEST(CallMonitor, APlayerThatStartedOverCountsFromZero) {
    Snapshots s;
    s.earlier.video = pictures(3000, Timestamp{}, 33ms);
    s.earlier.video->stream.frames.lost = 40;
    s.now.video = pictures(20, Timestamp{} + 10s, 33ms);  // a new player: fewer frames

    const CallReport report = compare(s.earlier, s.now);
    EXPECT_DOUBLE_EQ(report.video->fps, 20);
    EXPECT_EQ(report.video->lost, 0u);
}

TEST(CallMonitor, AnotherPeersPicturesCountFromZero) {
    Snapshots s;
    s.earlier.video = pictures(30, Timestamp{}, 33ms);
    s.earlier.video->peer = kAnn;
    s.earlier.video->stream.frames.lost = 4;
    s.now.video = pictures(50, Timestamp{} + 10s, 33ms);  // more, but Bob's
    s.now.video->peer = kBob;

    const CallReport report = compare(s.earlier, s.now);
    EXPECT_EQ(report.video->peer, kBob);
    EXPECT_DOUBLE_EQ(report.video->fps, 50);
    EXPECT_EQ(report.video->lost, 0u);
}

TEST(CallMonitor, SoundMadeUpIsFairThenPoor) {
    Snapshots s;
    s.earlier.sound = sound(500, 100ms);
    s.now.sound = sound(550, 110ms);  // 1% of the second
    EXPECT_EQ(compare(s.earlier, s.now).sound->verdict.quality, Quality::Fair);
    s.now.sound = sound(550, 140ms);  // 4%
    const CallReport report = compare(s.earlier, s.now);
    EXPECT_EQ(report.sound->concealed, 40ms);
    EXPECT_EQ(report.sound->verdict.quality, Quality::Poor);
    EXPECT_EQ(report.verdict.reason.rfind("sound: ", 0), 0u);
}

TEST(CallMonitor, DtxSilenceIsGoodAndOnlyNothingComingForLongIsPoor) {
    Snapshots s;
    s.earlier.sound = sound(500, {});
    s.now.sound = sound(502, {});
    s.now.sound->jitter.unexplained = 380ms;  // between DTX's frames
    EXPECT_EQ(compare(s.earlier, s.now).sound->verdict.quality, Quality::Good);
    s.now.sound->jitter.unexplained = 1500ms;
    const CallReport report = compare(s.earlier, s.now);
    EXPECT_EQ(report.sound->verdict.quality, Quality::Poor);
    EXPECT_EQ(report.verdict.reason, "sound: no sound came for over a second");
}

TEST(CallMonitor, EachPeersVoiceAndCameraAreTheirOwn) {
    Snapshots s;
    s.earlier.voices = {{kAnn, sound(500, {})}, {kBob, sound(500, {})}};
    s.now.voices = {{kAnn, sound(550, {})}, {kBob, sound(550, 40ms)}};  // Bob's made up
    s.now.cameras = {{kBob, pictures(30, Timestamp{} + 10s, 33ms)}};
    s.now.cameras[kBob].peer = kBob;

    const CallReport report = compare(s.earlier, s.now);
    ASSERT_EQ(report.voices.size(), 2u);
    EXPECT_EQ(report.voices[0].peer, kAnn);
    EXPECT_EQ(report.voices[0].verdict.quality, Quality::Good);
    EXPECT_EQ(report.voices[1].peer, kBob);
    EXPECT_EQ(report.voices[1].verdict.quality, Quality::Poor);
    ASSERT_EQ(report.cameras.size(), 1u);
    EXPECT_EQ(report.cameras[0].peer, kBob);
    EXPECT_DOUBLE_EQ(report.cameras[0].fps, 30);
    EXPECT_EQ(report.verdict.reason,
              "voice " + kBob.short_hex() + ": sound made up for frames that never came");

    const std::vector<std::string> lines = report_lines(report);
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0].rfind("camera " + kBob.short_hex(), 0), 0u) << lines[0];
    EXPECT_EQ(lines[1].rfind("voice " + kAnn.short_hex(), 0), 0u) << lines[1];
    EXPECT_EQ(lines[2].rfind("voice " + kBob.short_hex(), 0), 0u) << lines[2];
    const std::vector<ReportSection> sections = report_sections(report);
    ASSERT_EQ(sections.size(), 4u);
    EXPECT_EQ(sections[1].title, "Camera of " + kBob.short_hex());
    EXPECT_EQ(sections[3].title, "Voice of " + kBob.short_hex());
}

TEST(CallMonitor, APathIsJudgedByWhatItLosesAndLetsExpire) {
    const PeerId peer = kAnn;
    Snapshots s;
    s.earlier.paths = {{peer, path(1'000'000, 0)}};
    s.now.paths = {{peer, path(1'970'000, 30'000)}};  // 3% of what was resolved

    CallReport report = compare(s.earlier, s.now);
    ASSERT_EQ(report.paths.size(), 1u);
    EXPECT_DOUBLE_EQ(report.paths.front().loss, 0.03);
    EXPECT_EQ(report.paths.front().rtt, 20ms);
    EXPECT_EQ(report.paths.front().verdict.quality, Quality::Fair);

    s.now.paths.front().second.live_expired = 2;
    report = compare(s.earlier, s.now);
    EXPECT_EQ(report.paths.front().verdict.quality, Quality::Poor);
    EXPECT_EQ(report.verdict.reason.rfind("path to ", 0), 0u);
}

TEST(CallMonitor, APathThroughARelaySaysSo) {
    Snapshots s;
    s.earlier.paths = {{kAnn, path(1'000'000, 0)}, {kBob, path(1'000'000, 0)}};
    s.now.paths = {{kAnn, path(2'000'000, 0)}, {kBob, path(2'000'000, 0)}};
    s.now.relayed = {kBob};

    const CallReport report = compare(s.earlier, s.now);
    ASSERT_EQ(report.paths.size(), 2u);
    EXPECT_FALSE(report.paths[0].relayed);
    EXPECT_TRUE(report.paths[1].relayed);
    const std::vector<std::string> lines = report_lines(report);
    EXPECT_EQ(std::ranges::count_if(lines,
                                    [](const std::string& line) {
                                        return line.find("relayed") != std::string::npos;
                                    }),
              1);
    const std::vector<ReportSection> sections = report_sections(report);
    EXPECT_EQ(sections.back().title, "Path to " + kBob.short_hex());
    EXPECT_EQ(sections.back().rows.front().second, "relayed by a server");
}

TEST(CallMonitor, TheCallIsTheWorstOfItsParts) {
    Snapshots s;
    s.earlier.sent = {screen(0, 0, 0)};
    s.now.sent = {screen(30, 30, 100'000)};
    std::get<VideoPublisherStats>(s.now.sent.front().publisher).skipped = 1;  // fair
    s.earlier.sound = sound(0, {});
    s.now.sound = sound(50, 100ms);  // poor

    const CallReport report = compare(s.earlier, s.now);
    EXPECT_EQ(report.sent.front().verdict.quality, Quality::Fair);
    EXPECT_EQ(report.verdict.quality, Quality::Poor);
    EXPECT_EQ(report.verdict.reason, "sound: sound made up for frames that never came");
}

TEST(CallMonitor, ALineAPartAndTheCallLast) {
    Snapshots s;
    s.earlier.sent = {screen(0, 0, 0)};
    s.now.sent = {screen(60, 60, 500'000, 60 * 5ms)};
    s.now.video = pictures(30, Timestamp{} + 10s, 33ms);
    s.now.sound = sound(50, {});

    const std::vector<std::string> lines = report_lines(compare(s.earlier, s.now));
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_NE(lines[0].find("sent screen"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find("1920x1080 libsvtav1"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find("4.00 Mbit/s (aim 5.00 Mbit/s)"), std::string::npos) << lines[0];
    EXPECT_NE(lines[1].find("pictures"), std::string::npos) << lines[1];
    EXPECT_NE(lines[1].find("frame  33."), std::string::npos) << lines[1];
    EXPECT_NE(lines[2].find("sound"), std::string::npos) << lines[2];
    EXPECT_EQ(lines[3].rfind("call", 0), 0u) << lines[3];
    EXPECT_NE(lines[3].find("good"), std::string::npos) << lines[3];
}

TEST(CallMonitor, TheTreeIsShownWhereThereIsOne) {
    Snapshots s;
    s.earlier.sent = {screen(0, 0, 0)};
    s.now.sent = {screen(60, 60, 500'000)};
    s.now.sent[0].session.subscribers = 3;
    s.now.sent[0].session.relayed = 2;
    s.earlier.video = pictures(0, Timestamp{} + 10s, 33ms);
    s.now.video = pictures(30, Timestamp{} + 10s, 33ms);
    s.now.video->stream.relaying_to = 2;
    s.now.video->stream.relayed_bytes = 1'000'000;

    const CallReport report = compare(s.earlier, s.now);
    EXPECT_EQ(report.sent.front().relayed, 2u);
    ASSERT_TRUE(report.video);
    EXPECT_EQ(report.video->passed_on_to, 2u);
    EXPECT_DOUBLE_EQ(report.video->passed_on_bitrate, 8'000'000);
    const std::vector<std::string> lines = report_lines(report);
    EXPECT_NE(lines[0].find("viewers 3 (2 relayed)"), std::string::npos) << lines[0];
    EXPECT_NE(lines[1].find("passing on to 2 at 8.00 Mbit/s"), std::string::npos) << lines[1];

    // And not where there is none.
    s.now.sent[0].session.relayed = 0;
    s.now.video->stream.relaying_to = 0;
    const std::vector<std::string> plain = report_lines(compare(s.earlier, s.now));
    EXPECT_NE(plain[0].find("viewers 3  "), std::string::npos) << plain[0];
    EXPECT_EQ(plain[1].find("passing on"), std::string::npos) << plain[1];
}

TEST(CallMonitor, TheTextIsTheLinesUnderTheSecond) {
    Snapshots s;
    s.now.sound = sound(50, {});
    const CallReport report = compare(s.earlier, s.now);
    const std::vector<std::string> lines = report_lines(report);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(report_text(report, 12), "[  12s] " + lines[0] + "\n        " + lines[1] + "\n");
}

TEST(CallMonitor, ASectionAPartAndTheCallFirst) {
    Snapshots s;
    s.earlier.sent = {screen(0, 0, 0)};
    s.now.sent = {screen(60, 60, 500'000, 60 * 5ms)};
    s.earlier.voices = {{kAnn, sound(0, {})}};
    s.now.voices = {{kAnn, sound(50, 100ms)}};

    const std::vector<ReportSection> sections = report_sections(compare(s.earlier, s.now));
    ASSERT_EQ(sections.size(), 3u);
    EXPECT_EQ(sections[0].title, "Call");
    EXPECT_EQ(sections[0].verdict.quality, Quality::Poor);
    EXPECT_EQ(sections[1].title, "Sent: screen");
    EXPECT_EQ(sections[1].verdict.quality, Quality::Good);
    const auto row = [&](const ReportSection& section, std::string_view label) {
        for (const auto& [name, value] : section.rows) {
            if (name == label) return value;
        }
        return std::string("(none)");
    };
    EXPECT_EQ(row(sections[1], "Size"), "1920x1080");
    EXPECT_EQ(row(sections[1], "Bitrate"), "4.00 Mbit/s (aim 5.00 Mbit/s)");
    EXPECT_EQ(row(sections[1], "Encode"), "5.0 ms");
    EXPECT_EQ(sections[2].title, "Voice of " + kAnn.short_hex());
    EXPECT_EQ(row(sections[2], "Made up"), "100.0 ms");
}

TEST(CpuMeter, CountsTheProcessCpuTime) {
    CpuMeter meter;
    const Duration start = process_cpu_time();
    const auto give_up = std::chrono::steady_clock::now() + 5s;
    volatile uint64_t sink = 0;
    while (process_cpu_time() - start < 20ms && std::chrono::steady_clock::now() < give_up) {
        for (int i = 0; i < 100'000; ++i) sink = sink + i;
    }
    EXPECT_GE(process_cpu_time() - start, 20ms);
    EXPECT_GT(meter.take_interval(), 0.0);
    EXPECT_GT(meter.total(), 0.0);
}
