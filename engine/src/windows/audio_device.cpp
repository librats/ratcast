// Sound devices on Windows: WASAPI streams in shared mode, which need no
// permission beyond the user's leave for the microphone.

#include "audio/device_backend.h"
#include "windows/wasapi_audio.h"

namespace ratcast::backend {

Result<std::unique_ptr<AudioSource>> open_audio_capture(const AudioCaptureOptions& options) {
    return std::unique_ptr<AudioSource>(std::make_unique<wasapi::AudioCapture>(options));
}

Result<std::unique_ptr<AudioOutput>> open_audio_output(const AudioOutputOptions& options) {
    return std::unique_ptr<AudioOutput>(std::make_unique<wasapi::AudioPlayback>(options));
}

}  // namespace ratcast::backend
