// An MPEG-TS feed published on one session and played out on another, joined
// by a loopback: the bytes that go in come out, in order and a fixed latency
// later, whatever the reads they came in; what the network lost is skipped.

#include <gtest/gtest.h>

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

/// An input the test reads from by hand.
class ManualInput final : public TsInput {
public:
    Result<void> start(Sink sink, EndHandler) override {
        std::lock_guard lock(mutex_);
        sink_ = std::move(sink);
        return {};
    }
    void stop() override {
        std::lock_guard lock(mutex_);
        sink_ = nullptr;
    }
    void read(std::span<const uint8_t> bytes) {
        std::lock_guard lock(mutex_);
        if (sink_) sink_(bytes, SteadyClock::instance().now());
    }

private:
    std::mutex mutex_;
    Sink sink_;
};

/// An output that keeps what it is given, and when.
class RecordingOutput final : public TsOutput {
public:
    Result<void> write(std::span<const uint8_t> bytes) override {
        std::lock_guard lock(mutex_);
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        at_.push_back(SteadyClock::instance().now());
        return {};
    }
    std::vector<uint8_t> bytes() const {
        std::lock_guard lock(mutex_);
        return bytes_;
    }
    std::vector<Timestamp> at() const {
        std::lock_guard lock(mutex_);
        return at_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<uint8_t> bytes_;
    std::vector<Timestamp> at_;
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
    }

    /// Publish, and once announced play with `latency`.
    void publish_and_play(Duration latency) {
        publisher_ = std::make_unique<DataPublisher>(*sender_, DataPublisherConfig{});
        ASSERT_TRUE(publisher_->start(input_));
        ASSERT_TRUE(eventually([&] {
            std::lock_guard lock(mutex_);
            return announced_.has_value();
        }));
        std::lock_guard lock(mutex_);
        player_ = std::make_unique<DataPlayer>(*receiver_, sender_id_, *announced_, output_,
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

    std::mutex mutex_;
    std::optional<StreamDescription> announced_;
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
    ASSERT_TRUE(eventually([&] { return output_.bytes().size() >= feed.size(); }));
    EXPECT_EQ(output_.bytes(), feed);

    const DataPublisherStats sent = publisher_->stats();
    EXPECT_EQ(sent.packets, 100u);
    EXPECT_EQ(sent.skipped, 3u);
    // Each read's whole packets — five, or six with the one cut before —
    // in one frame.
    EXPECT_EQ(sent.frames, 19u);
    const DataPlayerStats played = player_->stats();
    EXPECT_EQ(played.delivery.delivered, 19u);
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
    ASSERT_TRUE(eventually([&] { return output_.at().size() >= 20; }));
    const std::vector<Timestamp> written = output_.at();
    for (size_t n = 0; n < 20; ++n) {
        // The latency, past the loopback's fastest delivery (well under a ms).
        EXPECT_GE(written[n] - read[n], 49ms) << "frame " << n;
        EXPECT_LE(written[n] - read[n], 80ms) << "frame " << n;
    }
}

TEST_F(DataPipelineTest, WhatTheNetworkLostIsSkipped) {
    lose_ = [](FrameNumber n) { return n == 5; };
    publish_and_play(20ms);
    for (size_t n = 0; n < 10; ++n) input_.read(packets(n * 6, 6));
    ASSERT_TRUE(eventually([&] { return player_->stats().delivery.delivered >= 9; }));
    std::vector<uint8_t> expected = packets(0, 30);
    const std::vector<uint8_t> rest = packets(36, 24);
    expected.insert(expected.end(), rest.begin(), rest.end());
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
