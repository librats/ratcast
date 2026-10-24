#include "ratcast/session/shared_uplink.h"

#include <algorithm>

namespace ratcast {

bool SharedUplink::update(const PeerId& peer, const PathSignal& signal, bool overused, size_t paths,
                          Timestamp now) {
    if (capacity_ && now - found_at_ > kMemory) capacity_.reset();
    if (signal.since > kStall) stalled_at_ = now;

    const auto [it, added] = paths_.try_emplace(peer);
    Path& path = it->second;
    if (added) path.first_seen = now;
    const bool rising = signal.queue_rise > kRise && path.rise > kRise;
    path.updated_at = now;
    path.carried = signal.carried.value_or(0);
    path.rise = signal.queue_rise;

    // The paths of now — those heard from within the last second — and what
    // they carried together.
    const auto recent = [now](const Path& p) { return now - p.updated_at <= kTogether; };
    uint64_t carried = 0;
    for (const auto& [id, other] : paths_) {
        if (recent(other)) carried += other.carried;
    }

    if (stalled_at_ && now - *stalled_at_ <= kTogether) {
        for (auto& [id, other] : paths_) other.full_at.reset();
        return false;
    }
    // Once the uplink is known, the copies are kept to their part of it: a
    // queue that fills it with them below that is somebody else's.
    if (capacity_ && double(carried) <= double(*capacity_) * kCopies) return false;
    if (!overused && !rising) return false;
    path.full_at = now;

    size_t full = 0;
    size_t young = 0;
    for (const auto& [id, other] : paths_) {
        if (!recent(other)) continue;
        if (other.full_at && now - *other.full_at <= kTogether) {
            ++full;
        } else if (now - other.first_seen <= kTogether) {
            ++young;
        }
    }
    if (paths < 2 || full < 2 || full + young < paths || carried == 0) return false;

    for (auto& [id, other] : paths_) other.full_at.reset();  // one filling, counted once
    capacity_ = std::max(capacity_.value_or(0), carried);
    found_at_ = now;
    return true;
}

}  // namespace ratcast
