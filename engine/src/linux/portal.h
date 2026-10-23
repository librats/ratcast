#pragma once

/**
 * @file portal.h
 * @brief Asking xdg-desktop-portal for a screen cast, and to keep the screen on.
 *
 * The portal is how a Linux desktop lets an application capture the screen:
 * the application asks over D-Bus, the desktop shows its own picker, and the
 * answer is a PipeWire connection with access to just the node that shows
 * what the user chose. A restore token from an earlier cast stands in for the
 * picker, so the user is asked once.
 *
 * Each cast has a D-Bus connection of its own, so that the portal closes the
 * session even if the process dies without saying so; so has each inhibition.
 */

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>

#include "ratcast/core/result.h"

namespace ratcast::portal {

struct ScreenCastRequest {
    bool monitors = true;  ///< offer whole screens
    bool windows = true;   ///< offer single windows
    bool cursor = true;    ///< the compositor draws the pointer into the pictures
    std::string restore_token;
};

/// A screen cast the user granted. Destroying it ends the cast.
class ScreenCast {
public:
    virtual ~ScreenCast() = default;

    /// A connection to PipeWire that can see the node; owned by the cast.
    virtual int pipewire_fd() const noexcept = 0;
    virtual uint32_t node() const noexcept = 0;

    /// What to pass as ScreenCastRequest::restore_token to cast the same
    /// thing next time without asking; empty if the portal gave none.
    virtual const std::string& restore_token() const noexcept = 0;
};

/// Ask for a screen cast, and wait for the user to choose. Cancelled when the
/// user declined or `stop` was requested; Unavailable when there is no portal
/// or it cannot cast what was asked for.
Result<std::unique_ptr<ScreenCast>> start_screen_cast(const ScreenCastRequest& request,
                                                      std::stop_token stop = {});

/// The desktop not blanking, locking or saving the screen for idleness while
/// this lives. Destroying it ends the inhibition.
class Inhibition {
public:
    virtual ~Inhibition() = default;
};

/// Ask the portal to keep the screen on, saying `reason`. Unavailable when
/// there is no portal or it refused; Cancelled when `stop` was requested
/// before it answered.
Result<std::unique_ptr<Inhibition>> inhibit_idle(const std::string& reason,
                                                 std::stop_token stop = {});

}  // namespace ratcast::portal
