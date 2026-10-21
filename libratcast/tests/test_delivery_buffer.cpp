// DeliveryBuffer in virtual time: a sender of a frame every 10 ms, a path
// with a transit of the test's choosing, and a reader that takes whatever is
// due each millisecond, as a socket writer would.

#include "ratcast/stream/delivery_buffer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr Duration kEvery = 10ms;
const Timestamp kSent{100s};      // the sender's clock
const Timestamp kReceived{500s};  // the receiver's, unrelated

struct Delivered {
    FrameNumber number;
    Timestamp at;
};

class Simulation {
public:
    explicit Simulation(DeliveryBuffer::Config config = {.latency = 100ms}) : buffer_(config) {}

    /// How long frame `number` takes; nullopt for one that never comes.
    std::function<std::optional<Duration>(FrameNumber)> transit = [](FrameNumber) {
        return std::optional<Duration>(30ms);
    };

    Timestamp capture_of(FrameNumber number) const {
        return kSent + kEvery * static_cast<int64_t>(number);
    }
    /// Where frame `number` would arrive on the fastest path, on the receiver's clock.
    Timestamp fastest_arrival_of(FrameNumber number, Duration fastest = 30ms) const {
        return capture_of(number) - kSent + kReceived + fastest;
    }

    /// Send `frames` frames and read for `until` past the start.
    void run(FrameNumber frames, Duration until) {
        std::multimap<Timestamp, FrameNumber> arrivals;
        for (FrameNumber n = 0; n < frames; ++n) {
            if (const std::optional<Duration> t = transit(n)) {
                arrivals.emplace(capture_of(n) - kSent + kReceived + *t, n);
            }
        }
        auto next = arrivals.begin();
        for (Timestamp now = kReceived; now < kReceived + until; now += 1ms) {
            for (; next != arrivals.end() && next->first <= now; ++next) {
                EncodedFrame f;
                f.info.number = next->second;
                f.info.capture_time = capture_of(next->second);
                buffer_.push(f, now);
            }
            while (std::optional<EncodedFrame> f = buffer_.pop(now)) {
                delivered.push_back({f->info.number, now});
            }
        }
    }

    std::vector<FrameNumber> numbers() const {
        std::vector<FrameNumber> out;
        for (const Delivered& d : delivered) out.push_back(d.number);
        return out;
    }

    DeliveryBuffer& buffer() { return buffer_; }
    std::vector<Delivered> delivered;

private:
    DeliveryBuffer buffer_;
};

}  // namespace

TEST(DeliveryBufferTest, HandsFramesOnAtAFixedDelayPastTheFastest) {
    Simulation sim;
    sim.run(100, 2s);
    ASSERT_EQ(sim.delivered.size(), 100u);
    for (FrameNumber n = 0; n < 100; ++n) {
        EXPECT_EQ(sim.delivered[n].number, n);
        EXPECT_EQ(sim.delivered[n].at, sim.fastest_arrival_of(n) + 100ms);
    }
    EXPECT_EQ(sim.buffer().stats().lost, 0u);
}

TEST(DeliveryBufferTest, EvensOutJitterWithinTheLatency) {
    Simulation sim;
    // 30 ms, and up to 90 more, each frame its own.
    sim.transit = [](FrameNumber n) {
        return std::optional<Duration>(30ms + Duration{(n * 7919 % 91) * 1000});
    };
    sim.run(200, 3s);
    ASSERT_EQ(sim.numbers().size(), 200u);
    for (FrameNumber n = 0; n < 200; ++n) {
        EXPECT_EQ(sim.delivered[n].number, n);  // in order, though they came out of it
        // The fastest is learnt as frames come: never later than its time.
        EXPECT_LE(sim.delivered[n].at, sim.fastest_arrival_of(n) + 100ms);
    }
    // Once frame 0's 30 ms is known, every frame at its time exactly.
    EXPECT_EQ(sim.delivered[150].at, sim.fastest_arrival_of(150) + 100ms);
}

TEST(DeliveryBufferTest, AFrameRepairedInTimeTakesItsPlace) {
    Simulation sim;
    // Frame 50 sent again a round trip later, still within the latency.
    sim.transit = [](FrameNumber n) {
        return std::optional<Duration>(n == 50 ? 30ms + 80ms : 30ms);
    };
    sim.run(100, 2s);
    std::vector<FrameNumber> expected(100);
    for (FrameNumber n = 0; n < 100; ++n) expected[n] = n;
    EXPECT_EQ(sim.numbers(), expected);
    EXPECT_EQ(sim.delivered[50].at, sim.fastest_arrival_of(50) + 100ms);
    EXPECT_EQ(sim.buffer().stats().lost, 0u);
}

TEST(DeliveryBufferTest, AFrameNotThereInTimeIsSkippedAndDroppedIfItComes) {
    Simulation sim;
    // Frame 20 never comes; frame 40 comes too late.
    sim.transit = [](FrameNumber n) -> std::optional<Duration> {
        if (n == 20) return std::nullopt;
        return n == 40 ? 30ms + 150ms : 30ms;
    };
    sim.run(100, 2s);
    const std::vector<FrameNumber> numbers = sim.numbers();
    EXPECT_EQ(numbers.size(), 98u);
    EXPECT_EQ(std::ranges::count(numbers, 20), 0);
    EXPECT_EQ(std::ranges::count(numbers, 40), 0);
    EXPECT_TRUE(std::ranges::is_sorted(numbers));
    // The frames after a gap keep their time: nothing waits for a lost one.
    EXPECT_EQ(sim.delivered[20].number, 21u);
    EXPECT_EQ(sim.delivered[20].at, sim.fastest_arrival_of(21) + 100ms);
    const DeliveryBuffer::Stats& stats = sim.buffer().stats();
    EXPECT_EQ(stats.lost, 2u);
    EXPECT_EQ(stats.late, 1u);
}

TEST(DeliveryBufferTest, MakesRoomByHandingOnEarly) {
    DeliveryBuffer buffer({.latency = 1s, .max_frames = 4});
    for (FrameNumber n = 0; n < 6; ++n) {
        EncodedFrame f;
        f.info.number = n;
        f.info.capture_time = kSent;
        buffer.push(f, kReceived);
    }
    EXPECT_EQ(buffer.next_due(), kReceived + 1s);
    std::vector<FrameNumber> early;
    while (std::optional<EncodedFrame> f = buffer.pop(kReceived)) early.push_back(f->info.number);
    EXPECT_EQ(early, (std::vector<FrameNumber>{0, 1}));
    EXPECT_EQ(buffer.stats().early, 2u);
    EXPECT_FALSE(buffer.pop(kReceived + 999ms));
    EXPECT_TRUE(buffer.pop(kReceived + 1s));
}

TEST(DeliveryBufferTest, DropsDuplicatesAndStartsAfreshOnReset) {
    DeliveryBuffer buffer({.latency = 50ms});
    EncodedFrame f;
    f.info.capture_time = kSent;
    buffer.push(f, kReceived);
    buffer.push(f, kReceived);
    EXPECT_EQ(buffer.stats().late, 1u);
    EXPECT_TRUE(buffer.pop(kReceived + 50ms));
    buffer.push(f, kReceived + 60ms);  // already handed on
    EXPECT_EQ(buffer.stats().late, 2u);

    buffer.reset();
    EXPECT_EQ(buffer.stats().received, 0u);
    EXPECT_FALSE(buffer.next_due());
    buffer.push(f, kReceived + 1s);
    EXPECT_EQ(buffer.next_due(), kReceived + 1s + 50ms);
}
