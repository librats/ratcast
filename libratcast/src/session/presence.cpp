#include "ratcast/session/presence.h"

#include <algorithm>

#include "session/plain_text.h"

namespace ratcast {

bool is_valid(const Presence& presence) noexcept {
    for (auto it = presence.decodes.begin(); it != presence.decodes.end(); ++it) {
        if (media_kind_of(*it) != MediaKind::Video ||
            std::find(presence.decodes.begin(), it, *it) != it)
            return false;
    }
    return presence.name.size() <= Presence::kMaxNameSize && is_plain_text(presence.name);
}

}  // namespace ratcast
