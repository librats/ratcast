// The pointer apart from the pictures where the engine cannot read it yet.

#include "ratcast/engine/cursor_source.h"

namespace ratcast {

Result<std::unique_ptr<CursorSource>> open_cursor_source(const CursorSourceOptions& /*options*/) {
    return fail(Errc::Unavailable);
}

}  // namespace ratcast
