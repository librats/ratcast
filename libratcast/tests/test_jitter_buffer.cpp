// JitterBuffer in virtual time: a sender of 20 ms frames, a path with a
// transit of the test's choosing, and a player that asks for more each time
// what it was given has played out — what a sound device does.

#include "ratcast/stream/jitter_buffer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;
using Kind = JitterBuffer::Next::Kind;

namespace {

constexpr Duration kFrame = 20ms;
const Timestamp kSent{100s};      // the sender's clock
const Timestamp kReceived{500s};  // the receiver's, unrelated

struct Played {
    Kind kind;
    Timestamp capture_time;
    Duration duration;
    std::optional<FrameNumber> frame;  ///< the frame decoded, or the one whose FEC a gap uses
    Duration delay{};
    Duration target{};
};

class Simulation {
public:
    explicit Simulation(JitterBuffer::Config config = {}) : buffer_(config) {}

    /// Frame `number` is sent at kSent + number * frame (+ `silence_before`
    /// for every frame from `silent_from` on) and takes `transit(number)` to
    /// arrive; nullopt for one that is lost.
    std::function<std::optional<Duration>(FrameNumber)> transit = [](FrameNumber) {
        return std::optional<Duration>(30ms);
    };
    Duration frame = kFrame;
    FrameNumber silent_from = ~FrameNumber{0};
    Duration silence = 0ms;

    Timestamp capture_of(FrameNumber number) const {
        return kSent + frame * static_cast<int64_t>(number) +
               (number >= silent_from ? silence : 0ms);
    }

    /// Send `frames` frames and play until the last has played, or the
    /// player's clock reaches `until` past the start.
    void run(Duration until, FrameNumber frames) {
        // Arrivals, by the receiver's clock.
        std::multimap<Timestamp, FrameNumber> arrivals;
        for (FrameNumber n = 0; n < frames; ++n) {
            if (const std::optional<Duration> t = transit(n)) {
                // The sender's clock runs kReceived - kSent behind.
                arrivals.emplace(capture_of(n) - kSent + kReceived + *t, n);
            }
        }
        Timestamp now = kReceived;
        auto next_arrival = arrivals.begin();
        while (now < kReceived + until) {
            for (; next_arrival != arrivals.end() && next_arrival->first <= now; ++next_arrival) {
                EncodedFrame f;
                f.info.number = next_arrival->second;
                f.info.capture_time = capture_of(next_arrival->second);
                f.info.keyframe = true;
                buffer_.push(f, next_arrival->first);
            }
            JitterBuffer::Next next = buffer_.next(now);
            Played p{next.kind,    next.capture_time, next.duration,
                     std::nullopt, buffer_.delay(),   buffer_.target()};
            if (next.frame) p.frame = next.frame->info.number;
            if (next.kind == Kind::Wait) {
                now += 10ms;  // a device quantum of silence
            } else {
                played.push_back(p);
                now += next.duration;
                if (next.kind == Kind::Frame && p.frame == frames - 1) return;
            }
        }
    }

    std::vector<FrameNumber> frames_played() const {
        std::vector<FrameNumber> numbers;
        for (const Played& p : played) {
            if (p.kind == Kind::Frame) numbers.push_back(*p.frame);
        }
        return numbers;
    }

    const JitterBuffer& buffer() const { return buffer_; }
    std::vector<Played> played;

private:
    JitterBuffer buffer_;
};

}  // namespace

TEST(JitterBufferTest, OnASteadyPathPlaysEveryFrameAtTheLeastDelay) {
    Simulation sim;
    sim.run(5s, 250);
    const std::vector<FrameNumber> frames = sim.frames_played();
    ASSERT_GE(frames.size(), 240u);
    for (size_t i = 0; i < frames.size(); ++i) EXPECT_EQ(frames[i], i);
    // No lateness but the 2 ms the histogram counts in, and a frame for FEC.
    EXPECT_EQ(sim.buffer().target(), 2ms + kFrame);
    EXPECT_LE(sim.buffer().delay(), sim.buffer().target() + 10ms);  // the device's quantum
    EXPECT_EQ(sim.buffer().stats().concealed, 0ms);
    EXPECT_EQ(sim.buffer().stats().late, 0u);
    EXPECT_EQ(sim.buffer().stats().lost, 0u);
}

TEST(JitterBufferTest, WaitsForTheFirstFrameAndThenTheTarget) {
    JitterBuffer buffer({});
    EXPECT_EQ(buffer.next(kReceived).kind, Kind::Wait);
    EncodedFrame f;
    f.info.capture_time = kSent;
    buffer.push(f, kReceived);
    EXPECT_EQ(buffer.next(kReceived + 5ms).kind, Kind::Wait);
    const JitterBuffer::Next next = buffer.next(kReceived + buffer.target());
    EXPECT_EQ(next.kind, Kind::Frame);
    EXPECT_EQ(next.capture_time, kSent);
}

TEST(JitterBufferTest, LearnsHowLongTheSendersFramesAre) {
    Simulation sim;
    sim.frame = 10ms;
    sim.run(1s, 100);
    EXPECT_EQ(sim.buffer().frame_duration(), 10ms);
    EXPECT_EQ(sim.buffer().stats().concealed, 0ms);
}

TEST(JitterBufferTest, RaisesItsTargetToKeepAJitteryPathInTime) {
    Simulation sim;
    // 30 ms, and up to 40 more: each frame late by its own amount.
    sim.transit = [](FrameNumber n) {
        return std::optional<Duration>(30ms + Duration{(n * 7919 % 41) * 1000});
    };
    sim.run(20s, 1000);
    const JitterBuffer::Stats& stats = sim.buffer().stats();
    EXPECT_GE(sim.buffer().target(), 36ms + kFrame);
    EXPECT_LE(sim.buffer().target(), 44ms + kFrame);
    // A few frames late while the target learns, none after.
    EXPECT_LT(stats.late + stats.lost, 20u);
    EXPECT_GE(stats.played, 970u);
}

TEST(JitterBufferTest, MakesUpALostFrameWithTheFecOfTheNext) {
    Simulation sim;
    sim.transit = [](FrameNumber n) {
        return n == 100 ? std::nullopt : std::optional<Duration>(30ms);
    };
    sim.run(4s, 200);
    // Where frame 100 would have played: a frame's worth made up, with 101.
    const auto at = std::ranges::find_if(
        sim.played, [](const Played& p) { return p.kind == Kind::Gap && p.frame == 101; });
    ASSERT_NE(at, sim.played.end());
    EXPECT_EQ(at->capture_time, sim.capture_of(100));
    EXPECT_EQ(at->duration, kFrame);
    EXPECT_EQ((at + 1)->frame, 101u);
    EXPECT_EQ((at + 1)->kind, Kind::Frame);
    EXPECT_EQ(sim.buffer().stats().lost, 1u);
    EXPECT_EQ(sim.buffer().stats().concealed, kFrame);
}

TEST(JitterBufferTest, PlaysReorderedFramesInOrder) {
    Simulation sim;
    // Every odd frame overtakes the even one before it.
    sim.transit = [](FrameNumber n) { return std::optional<Duration>(n % 2 ? 30ms : 45ms); };
    sim.run(4s, 200);
    const std::vector<FrameNumber> frames = sim.frames_played();
    for (size_t i = 0; i < frames.size(); ++i) EXPECT_EQ(frames[i], i);
    EXPECT_EQ(sim.buffer().stats().lost, 0u);
}

TEST(JitterBufferTest, DtxSilenceIsPlayedAsSilenceNotAsLoss) {
    Simulation sim;
    // 50 frames, then 400 ms with nothing sent, then the sound again.
    sim.silent_from = 50;
    sim.silence = 400ms;
    sim.run(3s, 100);
    const JitterBuffer::Stats& stats = sim.buffer().stats();
    EXPECT_EQ(stats.silence, 400ms);
    EXPECT_EQ(stats.concealed, 0ms);
    EXPECT_EQ(stats.lost, 0u);
    EXPECT_EQ(stats.played, 100u);
}

TEST(JitterBufferTest, ASpikeRaisesTheDelayAndSilenceBringsItBack) {
    Simulation sim({.memory = 1s});
    // Frames 100-109 come 120 ms later than the rest. A pause follows once
    // the spike is forgotten (some two seconds: ten late frames are a sixth of
    // those a second remembers, and fade below 3% by then) and before the
    // excess has cost a frame (a second after that).
    sim.transit = [](FrameNumber n) {
        return std::optional<Duration>(n >= 100 && n < 110 ? 150ms : 30ms);
    };
    sim.silent_from = 225;
    sim.silence = 1s;
    sim.run(10s, 300);
    const JitterBuffer::Stats& stats = sim.buffer().stats();
    EXPECT_GE(stats.expanded, 80ms);  // it waited for the late frames
    EXPECT_GE(stats.cut, 80ms);       // and took that out of the pause
    EXPECT_EQ(stats.dropped, 0u);     // rather than out of the sound
    EXPECT_LE(sim.buffer().delay(), sim.buffer().target() + 10ms);
}

TEST(JitterBufferTest, ADelayLongOverTargetCostsFramesWhereThereIsNoSilence) {
    Simulation sim({.memory = 1s});
    // A burst late by 200 ms, then a steady path without a pause.
    sim.transit = [](FrameNumber n) {
        return std::optional<Duration>(n >= 50 && n < 60 ? 230ms : 30ms);
    };
    sim.run(10s, 500);
    const JitterBuffer::Stats& stats = sim.buffer().stats();
    EXPECT_GT(stats.dropped, 0u);
    EXPECT_LE(sim.buffer().delay(), sim.buffer().target() + kFrame * 3 / 2);
}

TEST(JitterBufferTest, DropsDuplicatesAndFramesPastTheirTime) {
    JitterBuffer buffer({});
    const auto frame = [](FrameNumber n) {
        EncodedFrame f;
        f.info.number = n;
        f.info.capture_time = kSent + kFrame * static_cast<int64_t>(n);
        return f;
    };
    buffer.push(frame(0), kReceived);
    buffer.push(frame(0), kReceived);
    EXPECT_EQ(buffer.stats().late, 1u);

    const Duration target = buffer.target();
    ASSERT_EQ(buffer.next(kReceived + target).kind, Kind::Frame);       // 0
    ASSERT_EQ(buffer.next(kReceived + target + 20ms).kind, Kind::Gap);  // 1 is not there
    buffer.push(frame(1), kReceived + target + 30ms);
    EXPECT_EQ(buffer.stats().late, 2u);
}
