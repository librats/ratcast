// Sound devices where the engine has no backend yet (Windows and macOS until
// theirs land; Linux built without PipeWire).

#include "audio/device_backend.h"

namespace ratcast::backend {

Result<std::unique_ptr<AudioSource>> open_audio_capture(const AudioCaptureOptions&) {
    return fail(Errc::Unavailable);
}

Result<std::unique_ptr<AudioOutput>> open_audio_output(const AudioOutputOptions&) {
    return fail(Errc::Unavailable);
}

}  // namespace ratcast::backend
