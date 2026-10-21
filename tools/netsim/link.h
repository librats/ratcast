#pragma once

/**
 * @file link.h
 * @brief One direction of an emulated path: what Linux's netem does to a
 * packet, without the kernel.
 *
 * A packet that enters the link is lost, dropped at a full queue, or given
 * the time it leaves — netem's rules, so that a scenario reads the same to
 * `tc qdisc … netem` (bench/netem.py) and to ratcast-netsim:
 *
 *   - loss first: at random, or in bursts (Gilbert-Elliott: a good state that
 *     loses nothing and a bad one that loses everything, by default);
 *   - then the queue: past `limit` packets held, the packet is dropped;
 *   - then its time: `delay`, give or take `jitter` drawn for each packet on
 *     its own, so that packets overtake one another — unless the link has a
 *     `rate`, which sends them in order, each its own size at that rate after
 *     the one before; a `reorder` share leaves at once, overtaking the rest.
 *
 * Deterministic for a seed. Not thread-safe.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "ratcast/core/time.h"

namespace ratcast::netsim {

/// What the link carries besides a datagram's payload, and counts against
/// its rate: Ethernet, IPv4 and UDP headers, as netem does on a veth.
inline constexpr size_t kHeaderBytes = 14 + 20 + 8;

struct LinkConfig {
    enum class Jitter : uint8_t { Uniform, Normal };

    Duration delay{};
    Duration jitter{};  ///< spread of the delay: ± for uniform, σ for normal
    Jitter distribution = Jitter::Uniform;
    double loss = 0;  ///< share lost at random, 0..1
    /// Bursts of loss (Gilbert-Elliott), 0..1 each: entering the bad state
    /// from the good, leaving it, and the share lost in each.
    struct Bursts {
        double enter = 0;
        double leave = 1;
        double loss_bad = 1;
        double loss_good = 0;
    };
    std::optional<Bursts> bursts;
    double reorder = 0;     ///< share sent at once, without the delay, 0..1
    uint64_t rate_bps = 0;  ///< 0: no rate of its own
    size_t limit = 0;       ///< packets held at most; 0: no bound
};

/// netem's options, the subset scenarios use: `delay TIME [JITTER]
/// [distribution normal|uniform]`, `loss [random] PERCENT`, `loss gemodel P [R
/// [1-H [1-K]]]`, `reorder PERCENT`, `rate RATE` (bit, kbit, mbit, gbit), `limit
/// PACKETS`. Times in us, ms or s. Nothing at all is the link as it is, with no
/// queue of its own; anything is netem's queue of 1000 unless `limit` says
/// otherwise. Nullopt for what it cannot read, and why in `error`.
std::optional<LinkConfig> parse_link(std::string_view options, std::string* error = nullptr);

class Link {
public:
    struct Stats {
        uint64_t packets = 0;     ///< entered
        uint64_t bytes = 0;       ///< of those, payload
        uint64_t lost = 0;        ///< to loss, random or in bursts
        uint64_t overflowed = 0;  ///< dropped at a full queue
    };

    Link(LinkConfig config, uint64_t seed);

    /// From now on, for the packets that enter: those held keep their times.
    void reconfigure(const LinkConfig& config) { config_ = config; }

    /// A datagram of `bytes` enters at `now`: when it leaves, or nullopt if it
    /// never does.
    std::optional<Timestamp> enter(size_t bytes, Timestamp now);

    const Stats& stats() const noexcept { return stats_; }

private:
    bool chance(double share) { return share > 0 && unit_(random_) < share; }
    bool lose();
    Duration delay();

    LinkConfig config_;
    std::mt19937_64 random_;
    std::uniform_real_distribution<double> unit_{0.0, 1.0};
    std::normal_distribution<double> normal_{0.0, 1.0};
    bool bad_ = false;  ///< the bursts' state

    /// When each packet held leaves, soonest first.
    std::priority_queue<Timestamp, std::vector<Timestamp>, std::greater<>> held_;
    Timestamp latest_{};  ///< the last of them to leave
    Stats stats_;
};

}  // namespace ratcast::netsim
