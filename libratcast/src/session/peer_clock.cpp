#include "ratcast/session/peer_clock.h"

namespace ratcast {

void PeerClock::add(Timestamp sent, Timestamp at, Timestamp received) {
    if (received < sent) return;  // not a probe of this clock's
    const Duration round_trip = received - sent;
    const Sample sample{received, round_trip, (sent - at) + round_trip / 2};
    while (!least_.empty() && least_.back().round_trip >= round_trip) least_.pop_back();
    least_.push_back(sample);
    while (least_.front().received < received - kWindow) least_.pop_front();
}

std::optional<Duration> PeerClock::offset() const noexcept {
    if (least_.empty()) return std::nullopt;
    return least_.front().offset;
}

std::optional<Duration> PeerClock::round_trip() const noexcept {
    if (least_.empty()) return std::nullopt;
    return least_.front().round_trip;
}

}  // namespace ratcast
