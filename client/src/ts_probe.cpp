#include "ratcast/client/ts_probe.h"

#include <algorithm>
#include <utility>

namespace ratcast {

namespace {

// After the four-byte header: the number, then the time sent in µs.
constexpr size_t kNumberAt = 4;
constexpr size_t kSentAt = 12;

void put64(uint8_t* to, uint64_t value) noexcept {
    for (int i = 7; i >= 0; --i, value >>= 8) to[i] = static_cast<uint8_t>(value);
}

uint64_t get64(const uint8_t* from) noexcept {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = value << 8 | from[i];
    return value;
}

}  // namespace

void TsProbeWriter::fill(std::span<uint8_t> packets, Timestamp now) noexcept {
    for (size_t at = 0; at + kTsPacketSize <= packets.size(); at += kTsPacketSize, ++next_) {
        uint8_t* p = packets.data() + at;
        p[0] = kTsSyncByte;
        p[1] = kTsProbePid >> 8;
        p[2] = kTsProbePid & 0xFF;
        p[3] = static_cast<uint8_t>(0x10 | (next_ & 0x0F));  // payload only; the counter
        put64(p + kNumberAt, next_);
        put64(p + kSentAt, static_cast<uint64_t>(now.time_since_epoch().count()));
        std::fill(p + kSentAt + 8, p + kTsPacketSize, uint8_t{0xFF});
    }
}

TsProbeMeter::TsProbeMeter(Clock& clock) : clock_(clock) {}

void TsProbeMeter::start() {
    std::lock_guard lock(mutex_);
    started_ = clock_.now();
    finder_ = {};
    lowest_.reset();
    highest_ = 0;
    unique_ = 0;
    std::fill(seen_.begin(), seen_.end(), false);
    continuity_.reset();
    last_packet_.reset();
    total_ = {};
    interval_ = {};
}

void TsProbeMeter::take(std::span<const uint8_t> bytes, Timestamp received) {
    std::lock_guard lock(mutex_);
    const uint64_t skipped = finder_.take(bytes, [&](std::span<const uint8_t> run) {
        for (size_t at = 0; at < run.size(); at += kTsPacketSize) {
            packet(run.subspan(at, kTsPacketSize), received);
        }
    });
    for (TsProbeStats* stats : {&total_, &interval_}) {
        stats->bytes += bytes.size();
        stats->foreign += skipped;
    }
}

void TsProbeMeter::packet(std::span<const uint8_t> packet, Timestamp received) {
    const uint16_t pid = static_cast<uint16_t>((packet[1] & 0x1F) << 8 | packet[2]);
    if (pid != kTsProbePid) {
        total_.foreign += kTsPacketSize;
        interval_.foreign += kTsPacketSize;
        return;
    }
    const uint64_t number = get64(packet.data() + kNumberAt);
    const Timestamp sent{Duration(static_cast<int64_t>(get64(packet.data() + kSentAt)))};
    const uint8_t continuity = packet[3] & 0x0F;

    const bool duplicate = seen(number);
    const bool reordered = !duplicate && number < highest_;
    const bool jumped = continuity_ && continuity != ((*continuity_ + 1) & 0x0F) &&
                        continuity != *continuity_;  // a packet may come twice
    continuity_ = continuity;
    if (!total_.first_packet) total_.first_packet = received - started_;
    const Duration gap = last_packet_ ? received - *last_packet_ : Duration{};
    last_packet_ = received;

    for (TsProbeStats* stats : {&total_, &interval_}) {
        ++stats->packets;
        stats->duplicates += duplicate;
        stats->reordered += reordered;
        stats->discontinuities += jumped;
        stats->longest_gap = std::max(stats->longest_gap, gap);
        if (!duplicate) stats->latency.add(std::max(received - sent, Duration{}));
    }
}

bool TsProbeMeter::seen(uint64_t number) {
    if (!lowest_) {
        lowest_ = highest_ = number;
    } else if (number > highest_) {
        // The window moves on: what it held in the places of the new numbers
        // is forgotten.
        for (uint64_t n = highest_ + 1; n <= number && n <= highest_ + kWindow; ++n) {
            seen_[n % kWindow] = false;
        }
        highest_ = number;
    } else if (highest_ - number >= kWindow) {
        return true;  // too old to tell
    }
    lowest_ = std::min(*lowest_, number);
    auto bit = seen_[number % kWindow];
    if (bit) return true;
    bit = true;
    ++unique_;
    return false;
}

TsProbeStats TsProbeMeter::total() const {
    std::lock_guard lock(mutex_);
    TsProbeStats total = total_;
    if (lowest_) total.lost = highest_ - *lowest_ + 1 - unique_;
    return total;
}

TsProbeStats TsProbeMeter::take_interval() {
    std::lock_guard lock(mutex_);
    return std::exchange(interval_, {});
}

}  // namespace ratcast
