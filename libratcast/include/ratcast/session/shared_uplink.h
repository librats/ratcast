#pragma once

/**
 * @file shared_uplink.h
 * @brief What a sender's own uplink carries, which all its paths go through.
 *
 * Rate control follows each path on its own (rate_controller.h). A stream sent
 * to several peers goes once to each, and every copy goes through the
 * sender's uplink: each path then looks as wide as the uplink is, and the
 * copies together fill its queue — the network's, which delays everything
 * through it, a voice too, and then drops.
 *
 * So the uplink is found full when every path that carries copies says so at
 * once (within a second, at least two of them): overused — frames expired, or
 * a queue in front of it — or a queue building beyond it, its round trip
 * risen more than 25 ms over its least of the last ten seconds in two samples
 * running (a keyframe to every viewer at once lifts it for one). The rise comes
 * first: the uplink's queue grows before it overflows. A path that began
 * carrying copies less than a second ago has yet to say anything, and need
 * not. What the paths carried together over the last second is then what the
 * uplink carries, and the copies of rate-controlled streams share 85% of it —
 * the rest is for what else goes through it: voices, headers, repairs, and room
 * for a queue to empty — each in proportion to the most its stream may take.
 *
 * One path alone is that path's own, and tells nothing of the uplink. Nor do
 * they all, for a second after a sample that came late: this node was not
 * running (a stall of the whole machine), and frames expired on every path
 * waiting for it, with little carried.
 *
 * Once known, the uplink is the most it was found to carry, kept for a minute
 * after it was last found full. It is found full again only while the copies
 * carry more than their part of it, and then never narrower: the paths sample
 * on through a filling, carrying less as the copies come down, and a moment
 * of a stall or of somebody else's traffic — which would take whatever the
 * copies leave — would have them give up a little more each time. A narrower
 * uplink is rate control's to follow, path by path, which cuts where frames
 * expire; a minute on, what the uplink carries is forgotten, rate control
 * climbs past it carefully, as past any ceiling, and finds it again if it is
 * still there.
 *
 * Not thread-safe: MediaSession locks around it.
 */

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "ratcast/core/time.h"
#include "ratcast/export.h"
#include "ratcast/session/media_network.h"
#include "ratcast/session/rate_controller.h"

namespace ratcast {

class RATCAST_EXPORT SharedUplink {
public:
    /// Of what the uplink carries, the part the copies share.
    static constexpr double kCopies = 0.85;
    /// How close together the paths must say so to speak of the uplink.
    static constexpr Duration kTogether = std::chrono::seconds(1);
    /// A rise of the round trip that is a queue building.
    static constexpr Duration kRise = std::chrono::milliseconds(25);
    /// How long what it carries is kept after it was last found full.
    static constexpr Duration kMemory = std::chrono::seconds(60);
    /// A sample this long after the one before it is of a node that stalled.
    static constexpr Duration kStall = std::chrono::milliseconds(300);

    /// A sample of `peer`'s path, one of `paths` that carry copies of
    /// rate-controlled streams now; `overused`: one of those it carries
    /// overuses it. Returns whether the uplink was found full just now.
    bool update(const PeerId& peer, const PathSignal& signal, bool overused, size_t paths,
                Timestamp now);

    /// What the uplink is known to carry for media, bits per second; nullopt
    /// when it was never found full, or not for kMemory.
    std::optional<uint64_t> capacity() const noexcept { return capacity_; }

    /// `peer` is gone: what its path said no longer counts.
    void forget(const PeerId& peer) { paths_.erase(peer); }

private:
    struct Path {
        Timestamp first_seen{};
        Timestamp updated_at{};
        uint64_t carried = 0;              ///< over the last second, as of its last sample
        Duration rise{};                   ///< of its round trip, in its last sample
        std::optional<Timestamp> full_at;  ///< when it last said the uplink may be full
    };

    std::unordered_map<PeerId, Path, PeerId::Hash> paths_;
    std::optional<uint64_t> capacity_;
    Timestamp found_at_{};
    std::optional<Timestamp> stalled_at_;
};

}  // namespace ratcast
