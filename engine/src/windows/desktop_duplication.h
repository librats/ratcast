#pragma once

/**
 * @file desktop_duplication.h
 * @brief A screen through DXGI Desktop Duplication.
 *
 * What Windows.Graphics.Capture cannot do: capture whichever desktop takes
 * input, the secure one too — the lock and login screens, UAC's — for a
 * process allowed to (SYSTEM, in the user's session); and on Windows 10 no
 * yellow border. A thread of its own attaches to the input desktop, takes the
 * screen's changes as the system hands them out, and when the desktop
 * switches (or the display's mode changes, or a full-screen application takes
 * it) it duplicates the screen again, there. Where it may not (a user's
 * process before the secure desktop), it waits, sending nothing, and goes on
 * as the desktop comes back. A screen gone (unplugged) for a few seconds ends
 * the capture, its EndHandler told success, as Windows.Graphics.Capture's.
 *
 * Each picture goes through d3d11::TextureReader as Windows.Graphics.Capture's
 * do. A change the frame rate lets through only later is not lost: the last
 * one held back is sent when its time comes. The pointer is never drawn in
 * (the system hands it out apart: cursor_source.h). A rotated display comes
 * as the system lays it out, unrotated.
 */

#include <memory>
#include <string_view>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/screen_capture.h"

namespace ratcast::dxgi {

/// A duplication of the screen `token` names ("monitor:\\.\DISPLAY1"), ready to
/// start. Fails with InvalidArgument for a screen not there, Unavailable where
/// the system will not duplicate it (a GPU or driver without it, a remote
/// session).
Result<std::unique_ptr<ScreenCapture>> duplicate_monitor(std::string_view token,
                                                         const ScreenCaptureOptions& options,
                                                         Clock& clock = SteadyClock::instance());

}  // namespace ratcast::dxgi
