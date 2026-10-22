// Screen capture where the engine has none yet (macOS until its backend
// lands; Linux built without PipeWire).

#include "ratcast/engine/screen_capture.h"

namespace ratcast {

Result<std::unique_ptr<ScreenCapture>> open_screen_capture(const ScreenCaptureOptions& options,
                                                           std::stop_token /*stop*/) {
    if (!options.monitors && !options.windows) return fail(Errc::InvalidArgument);
    return fail(Errc::Unavailable);
}

}  // namespace ratcast
