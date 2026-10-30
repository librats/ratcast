// Sound devices on Android: AAudio, which needs RECORD_AUDIO for the
// microphone; what the applications play through the screen's projection.

#include "android/aaudio.h"
#include "android/playback_capture.h"
#include "audio/device_backend.h"

namespace ratcast::backend {

Result<std::vector<AudioDeviceInfo>> list_audio_inputs() { return aaudio::list_devices(true); }

Result<std::vector<AudioDeviceInfo>> list_audio_outputs() { return aaudio::list_devices(false); }

Result<std::unique_ptr<AudioSource>> open_audio_capture(const AudioCaptureOptions& options) {
    if (options.source == AudioCaptureSource::Applications && options.node == 0)
        return std::unique_ptr<AudioSource>(std::make_unique<android::PlaybackCapture>(options));
    return std::unique_ptr<AudioSource>(std::make_unique<aaudio::AudioCapture>(options));
}

Result<std::unique_ptr<AudioOutput>> open_audio_output(const AudioOutputOptions& options) {
    return std::unique_ptr<AudioOutput>(std::make_unique<aaudio::AudioPlayback>(options));
}

}  // namespace ratcast::backend
