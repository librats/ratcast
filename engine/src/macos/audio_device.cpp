// Sound devices on macOS: Core Audio, which needs the user's leave for the
// microphone and for other applications' sound (the bundle's usage
// descriptions, app/resources/Info.plist.in).

#include "audio/device_backend.h"
#include "macos/core_audio.h"

namespace ratcast::backend {

Result<std::vector<AudioDeviceInfo>> list_audio_inputs() { return core_audio::list_devices(true); }

Result<std::vector<AudioDeviceInfo>> list_audio_outputs() {
    return core_audio::list_devices(false);
}

Result<std::unique_ptr<AudioSource>> open_audio_capture(const AudioCaptureOptions& options) {
    return std::unique_ptr<AudioSource>(std::make_unique<core_audio::AudioCapture>(options));
}

Result<std::unique_ptr<AudioOutput>> open_audio_output(const AudioOutputOptions& options) {
    return std::unique_ptr<AudioOutput>(std::make_unique<core_audio::AudioPlayback>(options));
}

}  // namespace ratcast::backend
