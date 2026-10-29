#pragma once

/**
 * @file screen_share.h
 * @brief Sharing a screen or a window, with the sound the computer plays.
 *
 * Two steps, as a UI takes them: open_screen_share() asks the desktop what to
 * share — its own dialog, which waits on the user, so off the UI thread — and
 * opens the computer's sound beside it: what the applications play, never this
 * one's own, so that a call's voices do not go back out with the film; publish_screen_share() puts
 * both on a call. The desktop's choice may be kept in a data directory, so the next share of the
 * same kind asks no more where the desktop allows that (GNOME, KDE, Windows, macOS 15.2 on): for a
 * sender run unattended; the app asks every time.
 *
 * The pictures go as a rate-controlled video stream (kScreenStream), the
 * sound as music: stereo, full band, no DTX or FEC (kScreenSoundStream).
 * unpublish_screen_share() stops both, and the call goes on.
 */

#include <memory>
#include <stop_token>
#include <string>

#include "ratcast/client/audio_publisher.h"
#include "ratcast/client/call.h"
#include "ratcast/client/video_publisher.h"
#include "ratcast/core/result.h"
#include "ratcast/engine/audio_source.h"
#include "ratcast/engine/video_codec.h"
#include "ratcast/engine/video_source.h"

namespace ratcast {

inline constexpr StreamId kScreenStream = 1;
inline constexpr StreamId kScreenSoundStream = 2;

struct ScreenShareConfig {
    bool window = false;  ///< a single window, not a whole screen
    bool sound = true;    ///< and what the other applications play
    /// The pictures kept in GPU memory, for a hardware encoder to read where
    /// they are, where the system can (ScreenCaptureOptions::gpu).
    bool gpu = true;
    /// Where the desktop's choice is kept, one file per kind; "": asked each
    /// time.
    std::string data_dir = {};
    /// Codec and encoder, the most rate control goes up to and the frame rate
    /// at most; the size comes from the screen.
    VideoEncoderConfig video = default_video();
    uint32_t audio_bitrate = 128'000;
    /// How long a frame is worth delivering.
    Duration latency = std::chrono::milliseconds(200);

    static VideoEncoderConfig default_video() {
        VideoEncoderConfig video;
        video.bitrate = 8'000'000;
        return video;
    }
};

/// What is shared: the screen's pictures, and its sound if asked.
struct ScreenShareSources {
    std::unique_ptr<VideoSource> pictures = {};
    std::unique_ptr<AudioSource> sound = {};  ///< none: no sound shared
};

/// Ask the desktop for a screen or a window (the dialog, unless the choice
/// kept from last time stands) and open the other applications' sound. Fails with
/// Cancelled when the user declined or `cancel` was requested, Unavailable
/// when this system cannot capture the screen or its sound.
Result<ScreenShareSources> open_screen_share(const ScreenShareConfig& config,
                                             std::stop_token cancel = {});

/// Publish `sources` on `call` as `config` says. Fails as Call::publish does;
/// then nothing is published.
Result<void> publish_screen_share(Call& call, ScreenShareSources sources,
                                  const ScreenShareConfig& config);

/// Stop sharing on `call`: the pictures and the sound withdrawn.
void unpublish_screen_share(Call& call);

}  // namespace ratcast
