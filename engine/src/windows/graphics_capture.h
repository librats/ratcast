#pragma once

/**
 * @file graphics_capture.h
 * @brief Screens and windows through Windows.Graphics.Capture.
 *
 * The user chooses in the system's own picker, which offers screens and
 * windows alike: what is offered cannot be narrowed to one kind. What was
 * chosen is kept as a restore token that names it in words that outlast the
 * process — a screen by its display device ("monitor:\\.\DISPLAY1"), a window
 * by its program and window class ("window:C:\...\app.exe|ClassName") — so
 * the next capture finds the same screen, or that program's window, unasked.
 *
 * Pictures arrive as BGRA textures in GPU memory, at most two at a time; each
 * that the frame pacer lets through is copied into memory the CPU reads and
 * converted from there into the format asked for. The size follows the screen
 * or window as it changes. The capture ends, with success, when the window
 * closes or the screen goes.
 */

#include <windows.h>

#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/screen_capture.h"

namespace ratcast::wgc {

/// Whether this system can capture screens and windows (Windows 10 1903 on).
bool supported();

/// What a restore token says of `monitor` or `window`; empty when it cannot be
/// said (a window whose program cannot be read).
std::string restore_token(HMONITOR monitor);
std::string restore_token(HWND window);

/// What `token` names, if it is still there: a screen of that device, or the
/// topmost visible window of that program and class.
HMONITOR find_monitor(std::string_view token);
HWND find_window(std::string_view token);

/// A capture of `monitor` or `window`, ready to start.
Result<std::unique_ptr<ScreenCapture>> capture_monitor(HMONITOR monitor,
                                                       const ScreenCaptureOptions& options,
                                                       Clock& clock = SteadyClock::instance());
Result<std::unique_ptr<ScreenCapture>> capture_window(HWND window,
                                                      const ScreenCaptureOptions& options,
                                                      Clock& clock = SteadyClock::instance());

/// Ask the user in the system's picker. Fails with Cancelled when the user
/// closes it or `stop` is requested, Unavailable where there is no picker.
Result<std::unique_ptr<ScreenCapture>> pick(const ScreenCaptureOptions& options,
                                            std::stop_token stop,
                                            Clock& clock = SteadyClock::instance());

}  // namespace ratcast::wgc
