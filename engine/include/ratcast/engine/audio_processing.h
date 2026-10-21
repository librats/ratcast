#pragma once

/**
 * @file audio_processing.h
 * @brief A voice made fit to send: the speaker's echo taken out, noise
 * suppressed, its level evened, and whether it is speaking.
 *
 * An AudioProcessor sits between the microphone and the encoder, and hears
 * what the speaker plays — the far side, which comes back to the microphone
 * as echo. Its two sides run on two threads, the capture's and the output's,
 * each in blocks of any length; inside it works in 10 ms, so a block from the
 * microphone comes back once its last 10 ms are complete, the rest with the
 * next. How long the speaker's sound takes to reach the microphone it works
 * out from the times on both: when each block is played, when each was
 * captured.
 *
 * WebRTC's AudioProcessing (AEC3, its noise suppressor, AGC2) where the build
 * has it (RATCAST_WITH_WEBRTC_APM); Unavailable where not.
 */

#include <memory>
#include <optional>
#include <span>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/audio_frame.h"

namespace ratcast {

struct AudioProcessingConfig {
    AudioFormat capture{48'000, 1};   ///< the microphone's blocks
    AudioFormat playback{48'000, 2};  ///< the speaker's
    bool echo_cancellation = true;
    bool noise_suppression = true;
    bool gain_control = true;
};

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    /// What the speaker plays from `play_time` on, interleaved in the
    /// playback format: what to take out of the microphone. The output's
    /// thread; does not block.
    virtual void played(std::span<const float> samples, Timestamp play_time) = 0;

    /// A block from the microphone, in the capture format: what of it and
    /// the blocks before is complete, cleaned, stamped with when it was
    /// captured; nullopt while less than 10 ms waits. The capture's thread.
    virtual std::optional<AudioFrame> process(const AudioFrame& block) = 0;

    /// Whether the last 10 ms processed held a voice. Any thread.
    virtual bool voice() const noexcept = 0;
};

/// Fails with InvalidArgument for a format it cannot take (8–48 kHz, one
/// or two channels), Unavailable in a build without it.
Result<std::unique_ptr<AudioProcessor>> open_audio_processor(
    const AudioProcessingConfig& config, Clock& clock = SteadyClock::instance());

}  // namespace ratcast
