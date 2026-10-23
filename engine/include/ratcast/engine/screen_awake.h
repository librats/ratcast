#pragma once

/**
 * @file screen_awake.h
 * @brief The screen kept on while something is watched.
 *
 * A desktop blanks, locks or saves the screen after some minutes without
 * input, and nobody watching a film touches anything. While a ScreenAwake
 * lives, the desktop is asked not to: on Linux through xdg-desktop-portal's
 * Inhibit (the session's idleness), on Windows by a power request that the
 * display is needed. Elsewhere, or where the system says no, a warning is
 * logged and the screen is left to the system.
 *
 * Making and destroying one never waits on the desktop: it is asked on a
 * thread of the ScreenAwake's own.
 */

#include <memory>
#include <string>

namespace ratcast {

class ScreenAwake {
public:
    virtual ~ScreenAwake() = default;
    /// Whether the system has said yes, as of now.
    virtual bool held() const noexcept = 0;
};

/// Keep the screen on until the result is destroyed; `reason` is what the
/// desktop may show for it.
std::unique_ptr<ScreenAwake> keep_screen_awake(std::string reason);

}  // namespace ratcast
