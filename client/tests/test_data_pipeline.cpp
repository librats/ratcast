// An MPEG-TS feed published on one session and played out on another, joined
// by a loopback: the bytes that go in come out, in order and a fixed latency
// later, whatever the reads they came in; what the network lost is skipped.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "loopback.h"
#include "ratcast/client/data_player.h"
#include "ratcast/client/data_publisher.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

/// How much later than asked a timed wait may end — the delivery thread's, a
/// test's sleep — past a slow machine's odd stall: Windows' scheduler ticks
/// every 15.6 ms (docs/PLAN.md, L17); the hosted macOS runners' waits end tens
/// of milliseconds late.
#if defined(_WIN32)
constexpr Duration kWakeSlack = 16ms;
#elif defined(__APPLE__)
constexpr Duration kWakeSlack = 100ms;
#else
constexpr Duration kWakeSlack = 0ms;
#endif

PeerId make_peer(uint8_t seed) {
    std::array<uint8_t, 32> key{};
    key.fill(seed);
    return PeerId::from_public_key(key.data(), key.size());
}

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

/// Packet `n` of a test feed: the sync byte, then `n` in every byte after.
std::vector<uint8_t> packets(size_t from, size_t count) {
    std::vector<uint8_t> out;
    for (size_t n = from; n < from + count; ++n) {
        out.push_back(kTsSyncByte);
        out.insert(out.end(), kTsPacketSize - 1, static_cast<uint8_t>(n));
    }
    return out;
}

/// An input the test reads from by hand, and says by hand when it went quiet
/// or ended.
class ManualInput final : public TsInput {
public:
    Result<void> start(Sink sink, EndHandler ended, Duration quiet) override {
        std::lock_guard lock(mutex_);
        sink_ = std::move(sink);
        ended_ = std::move(ended);
        quiet_ = quiet;
        return {};
    }
    void stop() override {
        std::lock_guard lock(mutex_);
        sink_ = nullptr;
        ended_ = nullptr;
    }
    void read(std::span<const uint8_t> bytes) {
        std::lock_guard lock(mutex_);
        if (sink_) sink_(bytes, SteadyClock::instance().now());
    }
    void quiet() { read({}); }
    void end() {
        std::lock_guard lock(mutex_);
        if (ended_) ended_({});
        sink_ = nullptr;
        ended_ = nullptr;
    }
    Duration quiet_after() {
        std::lock_guard lock(mutex_);
        return quiet_;
    }

private:
    std::mutex mutex_;
    Sink sink_;
    EndHandler ended_;
    Duration quiet_{};
};

/// An output that keeps what it is given, and when.
class RecordingOutput final : public TsOutput {
public:
    Result<void> write(std::span<const uint8_t> bytes) override {
        std::lock_guard lock(mutex_);
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        at_.push_back(SteadyClock::instance().now());
        ends_.push_back(bytes_.size());
        return {};
    }
    std::vector<uint8_t> bytes() const {
        std::lock_guard lock(mutex_);
        return bytes_;
    }
    /// When each write was made.
    std::vector<Timestamp> at() const {
        std::lock_guard lock(mutex_);
        return at_;
    }
    /// When the byte at `offset` was written, if it was.
    std::optional<Timestamp> written_at(size_t offset) const {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < ends_.size(); ++i) {
            if (offset < ends_[i]) return at_[i];
        }
        return std::nullopt;
    }

private:
    mutable std::mutex mutex_;
    std::vector<uint8_t> bytes_;
    std::vector<Timestamp> at_;
    std::vector<size_t> ends_;  ///< where each write ended
};

class DataPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        sender_ = &loopback_.add(sender_id_, {});
        receiver_ = &loopback_.add(
            receiver_id_,
            SessionHandlers{
                .stream_announced =
                    [this](const PeerId&, const StreamDescription& description) {
                        std::lock_guard lock(mutex_);
                        announced_ = description;
                    },
                .stream_withdrawn = [this](const PeerId&, StreamId) { withdrawn_ = true; },
                .frame_received =
                    [this](const PeerId&, EncodedFrame frame) {
                        {
                            std::lock_guard lock(mutex_);
                            sizes_.push_back(frame.payload.size());
                        }
                        if (lose_ && lose_(frame.info.number)) return;
                        if (DataPlayer* p = player_ptr_.load()) p->push(std::move(frame));
                    },
            });
        loopback_.connect(sender_id_, receiver_id_);
    }

    void TearDown() override {
        player_ptr_ = nullptr;
        if (player_) player_->stop();
        if (publisher_) publisher_->stop();
        loopback_.stop();  // its handlers use what the test is about to destroy
    }

    /// Publish, and once announced play with `latency`.
    void publish_and_play(Duration latency) {
        publisher_ = std::make_unique<DataPublisher>(*sender_, DataPublisherConfig{});
        ASSERT_TRUE(publisher_->start(input_));
        ASSERT_TRUE(eventually([&] {
            std::lock_guard lock(mutex_);
            return announced_.has_value();
        }));
        // Not held past here: the loopback's thread takes it too — announced
        // again where publishing crossed the connection — and would deliver
        // nothing more, the subscription waited for included.
        const StreamDescription announced = [&] {
            std::lock_guard lock(mutex_);
            return *announced_;
        }();
        player_ = std::make_unique<DataPlayer>(*receiver_, sender_id_, announced, output_,
                                               DataPlayerConfig{.latency = latency});
        ASSERT_TRUE(player_->start());
        player_ptr_ = player_.get();
        // Frames go only to subscribers the sender knows of.
        ASSERT_TRUE(eventually([&] { return sender_->outgoing_stats(3)->subscribers == 1; }));
    }

    test::Loopback loopback_;
    const PeerId sender_id_ = make_peer(1);
    const PeerId receiver_id_ = make_peer(2);
    MediaSession* sender_ = nullptr;
    MediaSession* receiver_ = nullptr;

    ManualInput input_;
    RecordingOutput output_;
    std::unique_ptr<DataPublisher> publisher_;
    std::unique_ptr<DataPlayer> player_;
    std::atomic<DataPlayer*> player_ptr_{nullptr};
    std::function<bool(FrameNumber)> lose_;  ///< frames the receiver never sees

    std::vector<size_t> sizes() {
        std::lock_guard lock(mutex_);
        return sizes_;
    }

    std::mutex mutex_;
    std::optional<StreamDescription> announced_;
    std::vector<size_t> sizes_;  ///< of every frame received, lost ones too
    std::atomic<bool> withdrawn_{false};
};

}  // namespace

TEST_F(DataPipelineTest, TheFeedComesOutAsItWentInWhateverTheReads) {
    publish_and_play(30ms);
    {
        std::lock_guard lock(mutex_);
        EXPECT_EQ(announced_->codec, Codec::MpegTs);
        EXPECT_EQ(announced_->kind(), MediaKind::Data);
    }
    // 100 packets in reads of 1000 bytes, cut wherever that falls, after
    // three bytes that are no packet.
    const std::vector<uint8_t> feed = packets(0, 100);
    input_.read(std::array<uint8_t, 3>{1, 2, 3});
    for (size_t at = 0; at < feed.size(); at += 1000) {
        input_.read(std::span(feed).subspan(at, std::min<size_t>(1000, feed.size() - at)));
    }
    input_.quiet();  // the last four packets go
    ASSERT_TRUE(eventually([&] { return output_.bytes().size() >= feed.size(); }));
    EXPECT_EQ(output_.bytes(), feed);

    const DataPublisherStats sent = publisher_->stats();
    EXPECT_EQ(sent.packets, 100u);
    EXPECT_EQ(sent.skipped, 3u);
    EXPECT_EQ(sent.frames, 17u);  // sixteen of six, then four
    const DataPlayerStats played = player_->stats();
    EXPECT_EQ(played.delivery.delivered, 17u);
    EXPECT_EQ(played.delivery.lost, 0u);
    EXPECT_EQ(played.bytes, feed.size());
}

TEST_F(DataPipelineTest, FramesAreWrittenAFixedLatencyAfterTheyWereRead) {
    publish_and_play(50ms);
    std::vector<Timestamp> read;
    for (size_t n = 0; n < 20; ++n) {
        read.push_back(SteadyClock::instance().now());
        input_.read(packets(n * 6, 6));
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(eventually([&] { return output_.bytes().size() == 120 * kTsPacketSize; }));
    std::vector<Duration> held;
    for (size_t n = 0; n < 20; ++n) {
        // The latency, past the loopback's fastest delivery (well under a ms).
        const Duration frame_held = *output_.written_at(n * 6 * kTsPacketSize) - read[n];
        EXPECT_GE(frame_held, 49ms) << "frame " << n;
        held.push_back(frame_held);
    }
    // Not much longer, but for a slow machine's odd stall and its timers.
    std::ranges::nth_element(held, held.begin() + 10);
    EXPECT_LE(held[10], 60ms + kWakeSlack);
}

TEST_F(DataPipelineTest, WhatTheNetworkLostIsSkipped) {
    lose_ = [](FrameNumber n) { return n == 5; };
    publish_and_play(20ms);
    for (size_t n = 0; n < 10; ++n) input_.read(packets(n * 6, 6));
    std::vector<uint8_t> expected = packets(0, 30);
    const std::vector<uint8_t> rest = packets(36, 24);
    expected.insert(expected.end(), rest.begin(), rest.end());
    ASSERT_TRUE(eventually([&] { return output_.bytes().size() == expected.size(); }));
    EXPECT_EQ(output_.bytes(), expected);
    EXPECT_EQ(player_->stats().delivery.lost, 1u);
}

TEST_F(DataPipelineTest, StoppingWithdrawsTheStream) {
    publish_and_play(20ms);
    publisher_->stop();
    EXPECT_TRUE(eventually([&] { return withdrawn_.load(); }));
    // And again, from the start.
    ASSERT_TRUE(publisher_->start(input_));
    EXPECT_FALSE(publisher_->start(input_));
}

TEST_F(DataPipelineTest, WhatIsHeldWhenTheStreamEndsIsStillWrittenOut) {
    publish_and_play(200ms);
    for (size_t n = 0; n < 10; ++n) input_.read(packets(n * 6, 6));
    ASSERT_TRUE(eventually([&] { return player_->stats().delivery.received == 10; }));
    publisher_->stop();
    ASSERT_TRUE(eventually([&] { return withdrawn_.load(); }));
    EXPECT_TRUE(output_.bytes().empty());  // held for the latency still
    player_->finish();
    EXPECT_TRUE(eventually([&] { return output_.bytes().size() == 60 * kTsPacketSize; }));
    EXPECT_EQ(output_.bytes(), packets(0, 60));
}

TEST_F(DataPipelineTest, FramesGoFullWhateverTheReads) {
    publish_and_play(20ms);
    EXPECT_EQ(input_.quiet_after(), DataPublisherConfig{}.hold);
    // Datagrams of seven packets, as UDP feeds come: frames of six, the last
    // three waiting for more until the input goes quiet.
    for (size_t n = 0; n < 6; ++n) input_.read(packets(n * 7, 7));
    input_.read(packets(42, 3));
    ASSERT_TRUE(eventually([&] { return sizes().size() == 7; }));
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(sizes(), std::vector<size_t>(7, 6 * kTsPacketSize));
    input_.quiet();
    ASSERT_TRUE(eventually([&] { return output_.bytes().size() == 45 * kTsPacketSize; }));
    EXPECT_EQ(sizes().back(), 3 * kTsPacketSize);
    EXPECT_EQ(output_.bytes(), packets(0, 45));
}

TEST_F(DataPipelineTest, AFrameIsPlayedOutByItsFirstPacket) {
    publish_and_play(50ms);
    input_.read(packets(0, 6));  // a frame that waited for nothing: the fastest
    const Timestamp first = SteadyClock::instance().now();
    input_.read(packets(6, 3));
    std::this_thread::sleep_for(20ms);
    input_.read(packets(9, 3));  // the frame is full now
    const Timestamp full = SteadyClock::instance().now();
    ASSERT_TRUE(eventually([&] { return output_.bytes().size() == 12 * kTsPacketSize; }));
    // Out the latency after its first packet came in, as the feed went: with
    // the fastest, whose first packet came at the same time, as what waited
    // for the rest is not late — unless a slow machine's sleep had it full
    // only after that, when it goes at once. The fastest's own time is the
    // loopback's and the machine's: a stall in it moves both.
    const Timestamp fastest = *output_.written_at(0);
    const Timestamp written = *output_.written_at(6 * kTsPacketSize);
    EXPECT_GE(fastest - first, 49ms);
    EXPECT_LE(written - std::max(fastest, full), 10ms + kWakeSlack);
}

TEST_F(DataPipelineTest, WhatIsHeldGoesWhenPublishingStops) {
    publish_and_play(20ms);
    input_.read(packets(0, 2));
    publisher_->stop();
    ASSERT_TRUE(eventually([&] { return withdrawn_.load(); }));
    EXPECT_EQ(sizes(), std::vector<size_t>{2 * kTsPacketSize});
}

TEST_F(DataPipelineTest, WhatIsHeldGoesWhenTheInputEnds) {
    publish_and_play(20ms);
    input_.read(packets(0, 1));
    input_.end();
    ASSERT_TRUE(eventually([&] { return output_.bytes().size() == kTsPacketSize; }));
    EXPECT_EQ(sizes(), std::vector<size_t>{kTsPacketSize});
}

TEST_F(DataPipelineTest, FramesDueTogetherAreWrittenTogether) {
    publish_and_play(20ms);
    input_.read(packets(0, 18));  // three frames, read at once: due at once
    ASSERT_TRUE(eventually([&] { return output_.bytes().size() == 18 * kTsPacketSize; }));
    EXPECT_EQ(output_.bytes(), packets(0, 18));
    EXPECT_EQ(output_.at().size(), 1u);
    EXPECT_EQ(player_->stats().delivery.delivered, 3u);
}
