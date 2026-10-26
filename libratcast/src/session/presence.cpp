#include "ratcast/session/presence.h"

#include "session/plain_text.h"

namespace ratcast {

bool is_valid(const Presence& presence) noexcept {
    return presence.name.size() <= Presence::kMaxNameSize && is_plain_text(presence.name);
}

}  // namespace ratcast
