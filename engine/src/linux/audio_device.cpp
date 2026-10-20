// Sound devices on Linux: PipeWire streams, which need no portal — sound is
// the session's to use.

#include "audio/device_backend.h"
#include "linux/pipewire_audio.h"

namespace ratcast::backend {

Result<std::unique_ptr<AudioSource>> open_audio_capture(const AudioCaptureOptions& options) {
    return std::unique_ptr<AudioSource>(std::make_unique<pipewire::AudioCapture>(options));
}

Result<std::unique_ptr<AudioOutput>> open_audio_output(const AudioOutputOptions& options) {
    return std::unique_ptr<AudioOutput>(std::make_unique<pipewire::AudioPlayback>(options));
}

}  // namespace ratcast::backend
