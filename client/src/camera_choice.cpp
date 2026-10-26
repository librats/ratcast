#include "ratcast/client/camera_choice.h"

#include <algorithm>
#include <tuple>

namespace ratcast {

std::vector<PeerId> choose_cameras(std::span<const CameraOffer> offers, size_t most,
                                   Duration keep) {
    std::vector<const CameraOffer*> ranked;
    ranked.reserve(offers.size());
    for (const CameraOffer& offer : offers) ranked.push_back(&offer);
    if (most != 0 && ranked.size() > most) {
        // The latest to speak first — one shown as if it spoke `keep` later
        // — the one shown before the one not at the same time or never;
        // stable: then as offered.
        const auto rank = [keep](const CameraOffer* offer) {
            const Timestamp spoke = offer->spoke.value_or(Timestamp{});
            return std::tuple(offer->spoke.has_value(), offer->shown ? spoke + keep : spoke,
                              offer->shown);
        };
        std::ranges::stable_sort(
            ranked, [&](const CameraOffer* a, const CameraOffer* b) { return rank(a) > rank(b); });
        ranked.resize(most);
    }
    std::vector<PeerId> chosen;
    chosen.reserve(ranked.size());
    for (const CameraOffer* offer : ranked) chosen.push_back(offer->peer);
    return chosen;
}

}  // namespace ratcast
