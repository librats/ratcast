#pragma once

/**
 * @file in_call.h
 * @brief The system told that a call is on, for as long as it is.
 *
 * A desktop needs no telling. A phone does: on Android, while an InCall
 * lives, the audio mode is a call's, the call's sound (AudioOutputOptions::
 * call) goes to a headset where one is connected and to the loudspeaker
 * otherwise — never the earpiece a call's sound goes to by default — and
 * follows headsets as they come and go; audio focus is held, other
 * applications' sound stopped or ducked; and a foreground service, with its
 * notification, keeps the process while the app is in the background —
 * hearing the microphone and seeing the camera there too once the user has
 * allowed them, which update() tells the system of. It needs the Context
 * `ratcast::android::attach()` was given; with none (a tool under adb) a
 * warning is logged and nothing is done. Elsewhere it does nothing.
 */

#include <memory>

namespace ratcast {

class InCall {
public:
    virtual ~InCall() = default;
    /// Tell the system again what the call does: after the user allowed the
    /// microphone or the camera, so that it is heard or seen with the app in
    /// the background too.
    virtual void update() = 0;
};

/// The call on until the result is destroyed. Made on the thread that
/// destroys it; neither waits on the system.
std::unique_ptr<InCall> enter_call();

}  // namespace ratcast
