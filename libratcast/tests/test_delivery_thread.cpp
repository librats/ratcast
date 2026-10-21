// DeliveryThread in real time: what DeliveryBuffer decides (tested in virtual
// time on its own) handed on by a thread, woken when a frame is due.

#include "ratcast/stream/delivery_thread.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr Duration kLatency = 30ms;

EncodedFrame frame(FrameNumber number, Timestamp captured) {
    EncodedFrame f;
    f.info.number = number;
    f.info.capture_time = captured;
    f.info.keyframe = true;
    return f;
}

/// What the sink was handed: each call's frames, and when.
class Sink {
public:
    DeliveryThread::Sink handler() {
        return [this](std::span<const EncodedFrame> frames) {
            std::lock_guard lock(mutex_);
            std::vector<FrameNumber> call;
            for (const EncodedFrame& f : frames) call.push_back(f.info.number);
            calls_.push_back(call);
            times_.push_back(SteadyClock::instance().now());
            changed_.notify_all();
        };
    }

    /// Waits until `count` frames came, or a second passed.
    std::vector<FrameNumber> frames(size_t count) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, 1s, [&] { return total() >= count; });
        std::vector<FrameNumber> all;
        for (const auto& call : calls_) all.insert(all.end(), call.begin(), call.end());
        return all;
    }

    std::vector<std::vector<FrameNumber>> calls() {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    Timestamp first_at() {
        std::lock_guard lock(mutex_);
        return times_.front();
    }

private:
    size_t total() const {
        size_t n = 0;
        for (const auto& call : calls_) n += call.size();
        return n;
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::vector<FrameNumber>> calls_;
    std::vector<Timestamp> times_;
};

TEST(DeliveryThreadTest, HandsFramesOnInOrderOnceDue) {
    Sink sink;
    DeliveryThread delivery({.latency = kLatency}, sink.handler());
    delivery.start();

    // Captured before they arrive, as every frame is: a transit of 6 to 10 ms.
    const Timestamp captured = SteadyClock::instance().now() - 10ms;
    for (FrameNumber n : {2, 0, 1, 4, 3}) {
        delivery.push(frame(n, captured + 1ms * static_cast<int64_t>(n)));
    }
    EXPECT_EQ(sink.frames(5), (std::vector<FrameNumber>{0, 1, 2, 3, 4}));
    EXPECT_GE(sink.first_at() - captured, kLatency) << "handed on before its time";

    const DeliveryBuffer::Stats stats = delivery.stats();
    EXPECT_EQ(stats.received, 5u);
    EXPECT_EQ(stats.delivered, 5u);
    EXPECT_EQ(stats.lost, 0u);
}

TEST(DeliveryThreadTest, FramesDueTogetherGoInOneCall) {
    Sink sink;
    DeliveryThread delivery({.latency = kLatency}, sink.handler());
    delivery.start();

    const Timestamp captured = SteadyClock::instance().now();
    for (FrameNumber n = 0; n < 3; ++n) delivery.push(frame(n, captured));
    ASSERT_EQ(sink.frames(3).size(), 3u);
    EXPECT_EQ(sink.calls(), (std::vector<std::vector<FrameNumber>>{{0, 1, 2}}));
}

TEST(DeliveryThreadTest, AFrameNotThereInTimeIsSkipped) {
    Sink sink;
    DeliveryThread delivery({.latency = kLatency}, sink.handler());
    delivery.start();

    const Timestamp captured = SteadyClock::instance().now();
    delivery.push(frame(0, captured));
    delivery.push(frame(2, captured + 1ms));
    EXPECT_EQ(sink.frames(2), (std::vector<FrameNumber>{0, 2}));
    delivery.push(frame(1, captured));  // after its time
    std::this_thread::sleep_for(kLatency);
    EXPECT_EQ(sink.frames(2).size(), 2u);
    EXPECT_EQ(delivery.stats().lost, 1u);
    EXPECT_EQ(delivery.stats().late, 1u);
}

TEST(DeliveryThreadTest, FinishedItHandsOnWhatItHoldsThenEnds) {
    Sink sink;
    std::atomic<int> drained{0};
    std::vector<FrameNumber> handed_before_drained;
    DeliveryThread delivery({.latency = kLatency}, sink.handler(), [&] {
        handed_before_drained = sink.frames(0);
        ++drained;
    });
    delivery.start();

    const Timestamp captured = SteadyClock::instance().now();
    delivery.push(frame(0, captured));
    delivery.push(frame(1, captured + 5ms));
    delivery.finish();
    delivery.push(frame(2, captured + 6ms));  // nothing comes after the end

    EXPECT_EQ(sink.frames(2), (std::vector<FrameNumber>{0, 1}));
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (drained == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(drained, 1);
    EXPECT_EQ(handed_before_drained, (std::vector<FrameNumber>{0, 1}));
    delivery.stop();  // joins the thread that ended
    EXPECT_EQ(drained, 1);
}

TEST(DeliveryThreadTest, StoppedWhileFinishingItStillTellsDrained) {
    Sink sink;
    std::atomic<int> drained{0};
    DeliveryThread delivery({.latency = 10s}, sink.handler(), [&] { ++drained; });
    delivery.start();
    delivery.push(frame(0, SteadyClock::instance().now()));
    delivery.finish();
    delivery.stop();
    EXPECT_EQ(drained, 1);
    EXPECT_TRUE(sink.calls().empty()) << "what was held is dropped";
}

TEST(DeliveryThreadTest, StoppedItDropsWhatItHolds) {
    Sink sink;
    DeliveryThread delivery({.latency = 10s}, sink.handler());
    delivery.start();
    delivery.push(frame(0, SteadyClock::instance().now()));
    delivery.stop();
    EXPECT_TRUE(sink.calls().empty());

    // Started again, it starts afresh.
    delivery.start();
    EXPECT_EQ(delivery.stats().received, 0u);
    delivery.push(frame(7, SteadyClock::instance().now()));
    EXPECT_EQ(delivery.stats().received, 1u);
}

TEST(DeliveryThreadTest, ItsSinkMayStopIt) {
    std::atomic<int> calls{0};
    std::optional<DeliveryThread> delivery;
    delivery.emplace(DeliveryBuffer::Config{.latency = 1ms}, [&](std::span<const EncodedFrame>) {
        ++calls;
        delivery->stop();  // on its own thread: ends once this returns
    });
    delivery->start();
    const Timestamp captured = SteadyClock::instance().now();
    delivery->push(frame(0, captured));
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!delivery->ended() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(delivery->ended());
    delivery->push(frame(1, captured));
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(calls, 1);
    delivery.reset();  // joins the thread that ended
}

TEST(DeliveryThreadTest, NothingIsTakenUnlessStarted) {
    Sink sink;
    DeliveryThread delivery({.latency = kLatency}, sink.handler());
    delivery.push(frame(0, SteadyClock::instance().now()));
    EXPECT_EQ(delivery.stats().received, 0u);
}

}  // namespace
