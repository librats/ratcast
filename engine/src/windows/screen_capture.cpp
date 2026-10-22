// Screen capture on Windows: Windows.Graphics.Capture, what the restore token
// names if it is still there, else what the user picks.

#include "ratcast/engine/screen_capture.h"

#include "windows/graphics_capture.h"

namespace ratcast {

Result<std::unique_ptr<ScreenCapture>> open_screen_capture(const ScreenCaptureOptions& options,
                                                           std::stop_token stop) {
    if (!options.monitors && !options.windows) return fail(Errc::InvalidArgument);
    if (stop.stop_requested()) return fail(Errc::Cancelled);
    if (!wgc::supported()) return fail(Errc::Unavailable);
    if (options.monitors) {
        if (HMONITOR monitor = wgc::find_monitor(options.restore_token))
            return wgc::capture_monitor(monitor, options);
    }
    if (options.windows) {
        if (HWND window = wgc::find_window(options.restore_token))
            return wgc::capture_window(window, options);
    }
    return wgc::pick(options, std::move(stop));
}

}  // namespace ratcast
