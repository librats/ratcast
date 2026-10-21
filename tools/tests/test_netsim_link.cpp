// netsim's Link: netem's options read, and netem's rules kept — in virtual
// time, a packet every millisecond, over enough packets for the shares to
// settle.

#include "netsim/link.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace ratcast;
using namespace ratcast::netsim;
using namespace std::chrono_literals;

namespace {

constexpr size_t kPacket = 1200;
const Timestamp kStart{10s};

LinkConfig parsed(std::string_view options) {
    std::string error;
    const std::optional<LinkConfig> config = parse_link(options, &error);
    EXPECT_TRUE(config) << options << ": " << error;
    return config.value_or(LinkConfig{});
}

/// When each of `count` packets, one every `every`, left; nullopt for those lost.
std::vector<std::optional<Duration>> run(Link& link, size_t count, Duration every = 1ms) {
    std::vector<std::optional<Duration>> transits;
    for (size_t i = 0; i < count; ++i) {
        const Timestamp now = kStart + every * static_cast<int64_t>(i);
        const std::optional<Timestamp> leaves = link.enter(kPacket, now);
        transits.push_back(leaves ? std::optional(*leaves - now) : std::nullopt);
    }
    return transits;
}

double lost_share(const std::vector<std::optional<Duration>>& transits) {
    return double(std::count(transits.begin(), transits.end(), std::nullopt)) /
           double(transits.size());
}

TEST(NetsimLinkTest, ReadsTheOptionsScenariosUse) {
    LinkConfig c = parsed("delay 25ms 10ms distribution normal rate 1gbit");
    EXPECT_EQ(c.delay, 25ms);
    EXPECT_EQ(c.jitter, 10ms);
    EXPECT_EQ(c.distribution, LinkConfig::Jitter::Normal);
    EXPECT_EQ(c.rate_bps, 1'000'000'000u);
    EXPECT_EQ(c.limit, 1000u) << "netem's queue";

    c = parsed("delay 25ms loss gemodel 1% 25%");
    ASSERT_TRUE(c.bursts);
    EXPECT_DOUBLE_EQ(c.bursts->enter, 0.01);
    EXPECT_DOUBLE_EQ(c.bursts->leave, 0.25);
    EXPECT_DOUBLE_EQ(c.bursts->loss_bad, 1.0);
    EXPECT_DOUBLE_EQ(c.bursts->loss_good, 0.0);

    c = parsed("delay 25ms rate 6mbit limit 76 loss random 2% reorder 2%");
    EXPECT_EQ(c.rate_bps, 6'000'000u);
    EXPECT_EQ(c.limit, 76u);
    EXPECT_DOUBLE_EQ(c.loss, 0.02);
    EXPECT_DOUBLE_EQ(c.reorder, 0.02);

    c = parsed("delay 500us 1.5ms");
    EXPECT_EQ(c.delay, 500us);
    EXPECT_EQ(c.jitter, 1500us);
    EXPECT_EQ(c.distribution, LinkConfig::Jitter::Uniform) << "netem's own default";

    c = parsed("");
    EXPECT_EQ(c.limit, 0u) << "no shaping: no queue of its own";
}

TEST(NetsimLinkTest, RefusesWhatItCannotRead) {
    for (const char* options : {"delay", "delay 25", "delay 25min", "loss", "loss 120%", "loss two",
                                "rate 8mbps", "rate 0mbit", "limit 0", "limit lots", "reorder 2%",
                                "duplicate 1%", "delay 25ms distribution pareto", "loss gemodel"}) {
        std::string error;
        EXPECT_FALSE(parse_link(options, &error)) << options;
        EXPECT_FALSE(error.empty()) << options;
    }
}

TEST(NetsimLinkTest, DelaysEveryPacketAlike) {
    Link link(parsed("delay 25ms"), 1);
    for (const auto& transit : run(link, 1000)) EXPECT_EQ(transit, 25ms);
}

TEST(NetsimLinkTest, LosesItsShareAtRandom) {
    Link link(parsed("delay 25ms loss 2%"), 7);
    const auto transits = run(link, 200'000);
    EXPECT_NEAR(lost_share(transits), 0.02, 0.002);
    EXPECT_EQ(link.stats().lost, link.stats().packets - static_cast<uint64_t>(std::count_if(
                                                            transits.begin(), transits.end(),
                                                            [](auto t) { return t.has_value(); })));
}

TEST(NetsimLinkTest, LosesInBursts) {
    // Into the bad state 1% of the time, out of it 25%: p / (p + r) ≈ 3.8%
    // lost, in runs of 1 / r = 4 on average.
    Link link(parsed("delay 25ms loss gemodel 1% 25%"), 3);
    const auto transits = run(link, 400'000);
    EXPECT_NEAR(lost_share(transits), 0.01 / 0.26, 0.004);
    size_t runs = 0;
    size_t lost = 0;
    for (size_t i = 0; i < transits.size(); ++i) {
        if (transits[i]) continue;
        ++lost;
        if (i == 0 || transits[i - 1]) ++runs;
    }
    EXPECT_NEAR(double(lost) / double(runs), 4.0, 0.3);
}

TEST(NetsimLinkTest, JitterAloneLetsPacketsOvertake) {
    Link link(parsed("delay 25ms 10ms distribution normal"), 5);
    const auto transits = run(link, 20'000);
    double sum = 0;
    double squares = 0;
    size_t overtaken = 0;
    for (size_t i = 0; i < transits.size(); ++i) {
        const double ms = double(transits[i]->count()) / 1000;
        sum += ms;
        squares += ms * ms;
        // Sent 1 ms apart: one that takes over 1 ms longer than the next is overtaken.
        if (i + 1 < transits.size() && *transits[i] > *transits[i + 1] + 1ms) ++overtaken;
    }
    const double mean = sum / double(transits.size());
    EXPECT_NEAR(mean, 25.0, 0.3);
    EXPECT_NEAR(std::sqrt(squares / double(transits.size()) - mean * mean), 10.0, 0.3);
    EXPECT_GT(overtaken, transits.size() / 10);
}

TEST(NetsimLinkTest, JitterWithARateKeepsTheOrder) {
    Link link(parsed("delay 25ms 10ms distribution normal rate 1gbit"), 5);
    Timestamp last{};
    for (size_t i = 0; i < 20'000; ++i) {
        const Timestamp now = kStart + 1ms * static_cast<int64_t>(i);
        const Timestamp leaves = *link.enter(kPacket, now);
        EXPECT_GT(leaves, last) << "packet " << i;
        last = leaves;
    }
}

TEST(NetsimLinkTest, ReorderedPacketsLeaveAtOnce) {
    Link link(parsed("delay 25ms reorder 2%"), 11);
    const auto transits = run(link, 100'000);
    const auto early = std::count(transits.begin(), transits.end(), std::optional(Duration{}));
    EXPECT_NEAR(double(early) / double(transits.size()), 0.02, 0.002);
    EXPECT_EQ(std::count(transits.begin(), transits.end(), std::optional<Duration>(25ms)),
              static_cast<std::ptrdiff_t>(transits.size()) - early);
}

TEST(NetsimLinkTest, ARateSpacesPacketsByTheirSize) {
    // 1200 bytes and the headers, 1242 · 8 bits at 8 Mbit/s: 1.242 ms apart.
    Link link(parsed("delay 25ms rate 8mbit"), 1);
    std::vector<Timestamp> leaves;
    for (int i = 0; i < 100; ++i) leaves.push_back(*link.enter(kPacket, kStart));  // a burst
    EXPECT_EQ(leaves.front() - kStart, 25ms + 1242us);
    for (size_t i = 1; i < leaves.size(); ++i) EXPECT_EQ(leaves[i] - leaves[i - 1], 1242us);
}

TEST(NetsimLinkTest, AFullQueueDropsWhatComes) {
    // 6 Mbit/s and a stream of 8: the queue fills to its 76 and drops a quarter.
    Link link(parsed("delay 25ms rate 6mbit limit 76"), 1);
    const Duration every = std::chrono::duration_cast<Duration>(
        std::chrono::duration<double>((kPacket + kHeaderBytes) * 8 / 8e6));
    const auto transits = run(link, 20'000, every);
    EXPECT_NEAR(lost_share(transits), 0.25, 0.01);
    EXPECT_EQ(link.stats().lost, 0u);
    EXPECT_EQ(link.stats().overflowed,
              static_cast<uint64_t>(std::count(transits.begin(), transits.end(), std::nullopt)))
        << "every drop is the queue's";
    // Held a full queue's worth: delay, then 76 packets at the rate.
    EXPECT_LE(*transits.back(), 25ms + 76 * 1656us + 1ms);
}

TEST(NetsimLinkTest, AChangeTakesTheNextPacketsAndKeepsTheHeld) {
    Link link(parsed("delay 25ms rate 20mbit limit 256"), 1);
    const Timestamp held = *link.enter(kPacket, kStart);
    link.reconfigure(parsed("delay 25ms rate 4mbit limit 51"));
    const Timestamp next = *link.enter(kPacket, kStart);
    EXPECT_EQ(held - kStart, 25ms + 497us);  // 1242 · 8 at 20 Mbit/s, rounded
    EXPECT_EQ(next - held, 2484us) << "after it, at the new rate";
}

}  // namespace
