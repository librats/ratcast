#include "ratcast/client/screen_share.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/screen_capture.h"

namespace ratcast {
namespace {

/// Where the desktop's choice for a share of this kind is kept.
std::filesystem::path token_path(const ScreenShareConfig& config) {
    return std::filesystem::path(config.data_dir) /
           (config.window ? "window.token" : "screen.token");
}

}  // namespace

Result<ScreenShareSources> open_screen_share(const ScreenShareConfig& config,
                                             std::stop_token cancel) {
    ScreenCaptureOptions options;
    options.monitors = !config.window;
    options.windows = config.window;
    options.max_frame_rate = config.video.frame_rate_num;
    options.gpu = config.gpu;
    if (!config.data_dir.empty())
        std::getline(std::ifstream(token_path(config)), options.restore_token);

    Result<std::unique_ptr<ScreenCapture>> capture = open_screen_capture(options, cancel);
    if (!capture) return fail(capture.error());
    if (!config.data_dir.empty() && !(*capture)->restore_token().empty()) {
        std::error_code ignored;  // unkept, the next share asks again
        std::filesystem::create_directories(config.data_dir, ignored);
        std::ofstream(token_path(config)) << (*capture)->restore_token() << "\n";
    }

    ScreenShareSources sources{.pictures = std::move(*capture)};
    if (config.sound) {
        Result<std::unique_ptr<AudioSource>> sound =
            open_audio_capture({.format = {48'000, 2}, .source = AudioCaptureSource::Applications});
        if (!sound) return fail(sound.error());
        sources.sound = std::move(*sound);
    }
    return sources;
}

Result<void> publish_screen_share(Call& call, ScreenShareSources sources,
                                  const ScreenShareConfig& config) {
    VideoPublisherConfig video;
    video.stream = kScreenStream;
    video.label = config.window ? "window" : "screen";
    video.lifetime = config.latency;
    video.encoder = config.video;
    video.start_bitrate = std::min(video.start_bitrate, config.video.bitrate);
    if (Result<VideoPublisher*> published = call.publish(std::move(sources.pictures), video);
        !published) {
        return fail(published.error());
    }
    if (!sources.sound) return {};

    // Music: Opus codes it in CELT, which has no FEC, and silence is rare.
    AudioPublisherConfig sound;
    sound.stream = kScreenSoundStream;
    sound.label = "sound";
    sound.lifetime = config.latency;
    sound.encoder.content = AudioContent::Music;
    sound.encoder.bitrate = config.audio_bitrate;
    sound.encoder.dtx = false;
    sound.encoder.expected_loss = 0;
    if (Result<AudioPublisher*> published = call.publish(std::move(sources.sound), sound);
        !published) {
        call.unpublish(kScreenStream);
        return fail(published.error());
    }
    return {};
}

Result<void> switch_screen_share(Call& call, ScreenShareSources sources) {
    return call.switch_source(kScreenStream, std::move(sources.pictures));
}

void unpublish_screen_share(Call& call) {
    call.unpublish(kScreenStream);
    call.unpublish(kScreenSoundStream);
}

}  // namespace ratcast
