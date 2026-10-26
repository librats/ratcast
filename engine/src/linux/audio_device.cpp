// Sound devices on Linux: PipeWire streams, which need no portal — sound is
// the session's to use.

#include "audio/device_backend.h"
#include "linux/pipewire_applications.h"
#include "linux/pipewire_audio.h"
#include "linux/pipewire_registry.h"

#include <string_view>

namespace ratcast::backend {
namespace {

/// The devices among PipeWire's nodes whose class starts so: "Audio/Source"
/// (a virtual one, "Audio/Source/Virtual", too) or "Audio/Sink".
Result<std::vector<AudioDeviceInfo>> devices(std::string_view media_class) {
    Result<std::vector<pipewire::NodeInfo>> nodes = pipewire::list_nodes();
    if (!nodes) return fail(nodes.error());
    std::vector<AudioDeviceInfo> devices;
    for (pipewire::NodeInfo& node : *nodes) {
        if (!node.media_class.starts_with(media_class) || node.name.empty()) continue;
        std::string name = node.description.empty() ? node.name : std::move(node.description);
        devices.push_back({.id = std::move(node.name), .name = std::move(name)});
    }
    return devices;
}

}  // namespace

Result<std::vector<AudioDeviceInfo>> list_audio_inputs() { return devices("Audio/Source"); }

Result<std::vector<AudioDeviceInfo>> list_audio_outputs() { return devices("Audio/Sink"); }

Result<std::unique_ptr<AudioSource>> open_audio_capture(const AudioCaptureOptions& options) {
    if (options.node == 0 && options.source == AudioCaptureSource::Applications)
        return std::unique_ptr<AudioSource>(
            std::make_unique<pipewire::ApplicationsCapture>(options));
    return std::unique_ptr<AudioSource>(std::make_unique<pipewire::AudioCapture>(options));
}

Result<std::unique_ptr<AudioOutput>> open_audio_output(const AudioOutputOptions& options) {
    return std::unique_ptr<AudioOutput>(std::make_unique<pipewire::AudioPlayback>(options));
}

}  // namespace ratcast::backend
