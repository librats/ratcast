#include "ratcast/engine/audio_device.h"

#include "audio/device_backend.h"

namespace ratcast {
namespace {

bool is_valid(const AudioFormat& format, Duration latency) {
    using std::chrono::milliseconds;
    return format.sample_rate > 0 && format.channels > 0 && latency >= milliseconds(1) &&
           latency <= milliseconds(100);
}

}  // namespace

Result<std::vector<AudioDeviceInfo>> list_audio_inputs() { return backend::list_audio_inputs(); }

Result<std::vector<AudioDeviceInfo>> list_audio_outputs() { return backend::list_audio_outputs(); }

Result<std::unique_ptr<AudioSource>> open_audio_capture(const AudioCaptureOptions& options) {
    if (!is_valid(options.format, options.latency)) return fail(Errc::InvalidArgument);
    if (options.node == 0 && options.source == AudioCaptureSource::Applications &&
        options.format.channels > 2) {
        return fail(Errc::InvalidArgument);
    }
    return backend::open_audio_capture(options);
}

Result<std::unique_ptr<AudioOutput>> open_audio_output(const AudioOutputOptions& options) {
    if (!is_valid(options.format, options.latency)) return fail(Errc::InvalidArgument);
    return backend::open_audio_output(options);
}

}  // namespace ratcast
