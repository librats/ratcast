#pragma once

/**
 * @file projection.h
 * @brief The screen shared on Android: a MediaProjection the user allowed.
 *
 * Only Java reaches it (java/ratcast/engine/ScreenShare.java): the system's
 * dialog asks the user each time — a projection is never kept, and from
 * Android 14 no consent is used twice — and the projection is made once the
 * foreground service runs as a projection's. The screen's capture holds it,
 * and so does the capture of what the applications play, which needs one
 * (AudioPlaybackCapture): opened after the screen, it takes the projection
 * made last while that is held (current()). Let go by the last, it stops.
 */

#include <android/native_window.h>
#include <jni.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>

#include "ratcast/core/result.h"

namespace ratcast::android {

class Projection {
public:
    /// The user asked (the system's dialog, over the application), and the
    /// projection made if allowed. Fails with Cancelled when the user
    /// declined or `stop` was requested, Unavailable where there is no Java
    /// (a process under `adb shell`) or the projection cannot be made.
    static Result<std::shared_ptr<Projection>> ask(std::stop_token stop);

    /// The projection made last, while something holds it; else null.
    static std::shared_ptr<Projection> current();

    /// Over ScreenShare `share` (a global reference, now the projection's).
    explicit Projection(jobject share) : share_(share) {}
    Projection(const Projection&) = delete;
    Projection& operator=(const Projection&) = delete;
    ~Projection();

    struct Screen {
        uint32_t width = 0;  ///< as it is turned now
        uint32_t height = 0;
        int32_t dpi = 0;
    };
    /// The screen as it is now.
    std::optional<Screen> screen() const;

    /// The screen mirrored into `window`, `width` x `height` (its picture
    /// fitted in, its proportions kept); again into another as it turns.
    bool show(ANativeWindow* window, uint32_t width, uint32_t height, int32_t dpi) const;
    /// Mirrored nowhere, until shown again.
    void hide() const;

    /// Whether the projection was stopped: by the user, or by another.
    bool stopped() const;

    /// An AudioRecord, started, reading float samples of what the other
    /// applications play: a global reference, or null if it cannot be made
    /// (no RECORD_AUDIO).
    jobject sound(uint32_t sample_rate, uint32_t channels) const;

private:
    jobject share_;
};

}  // namespace ratcast::android
