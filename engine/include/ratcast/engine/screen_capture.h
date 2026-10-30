#pragma once

/**
 * @file screen_capture.h
 * @brief Capturing a screen or a single window.
 *
 * What is captured is the user's choice, made in the system's own picker —
 * on Linux the desktop portal's dialog, the only way under Wayland; on
 * Windows Windows.Graphics.Capture's, which offers screens and windows alike
 * whatever the options say; on macOS ScreenCaptureKit's, in an application
 * (a process with none captures the main screen); on Android MediaProjection's
 * consent, the whole screen — or one application where Android 14 offers it —
 * whatever the options say, in an application only — so opening a capture can
 * take as long as the user takes to choose. Run it off the UI thread; `stop` calls it off.
 *
 * A restore token from an earlier capture stands in for the picker: pass it
 * back to capture the same screen or window again without asking, where the
 * system supports that (GNOME and KDE do; on Windows a token names a screen by
 * its display, a window by its program and class; on macOS a screen by its
 * display's UUID, a window by its application, from macOS 15.2 on; Android
 * asks every time).
 *
 * Pictures come converted into the format asked for, with the pointer drawn
 * in, no faster than `max_frame_rate`. A compositor sends a picture only when
 * the screen changes, so a still screen sends none. The capture ends by
 * itself — the EndHandler hears it — when the window closes or the user stops
 * sharing from the desktop.
 */

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>

#include "ratcast/core/result.h"
#include "ratcast/engine/video_frame.h"
#include "ratcast/engine/video_source.h"

namespace ratcast {

struct ScreenCaptureOptions {
    bool monitors = true;  ///< offer whole screens
    bool windows = true;   ///< offer single windows
    bool cursor = true;    ///< draw the pointer into the pictures

    PixelFormat format = PixelFormat::NV12;
    ColorSpace color_space;  ///< of YUV pictures
    /// NV12 pictures may stay in GPU memory (VideoFrame::gpu()), for an
    /// encoder that reads them there (macOS: the capture's own IOSurfaces);
    /// where the GPU cannot convert them (Windows' software device; Linux,
    /// until DMA-BUF) they come in memory.
    bool gpu = false;
    uint32_t max_frame_rate = 60;  ///< 0 for as many as the screen sends

    /// From an earlier capture's restore_token(): the same choice, unasked.
    std::string restore_token;
};

class ScreenCapture : public VideoSource {
public:
    /// What to pass as ScreenCaptureOptions::restore_token next time to
    /// capture the same thing without asking; empty when there is none.
    virtual const std::string& restore_token() const noexcept = 0;
};

/// Let the user choose a screen or window and get ready to capture it. Fails
/// with Cancelled when the user declined or `stop` was requested, Unavailable
/// when this system cannot capture the screen, InvalidArgument for options
/// that offer nothing to choose.
Result<std::unique_ptr<ScreenCapture>> open_screen_capture(const ScreenCaptureOptions& options,
                                                           std::stop_token stop = {});

}  // namespace ratcast
