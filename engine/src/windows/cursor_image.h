// A Windows cursor's picture as RGBA (cursor_source.h). Apart from the source
// so that tests can hand it cursors of their own making.

#pragma once

#include <windows.h>

#include <optional>

#include "ratcast/engine/cursor_source.h"

namespace ratcast::windows {

/// `cursor`'s image, its id left 0: alpha where the cursor has it; else its
/// mask's, with the pixels that invert the screen (monochrome I-beams, a
/// colour cursor's masked colours) black with a white edge, as nothing can
/// invert what is under it on the other side. nullopt when it cannot be read
/// or is larger than kMaxCursorSide.
std::optional<CursorImage> read_cursor_image(HCURSOR cursor);

}  // namespace ratcast::windows
