#pragma once

/**
 * @file voice.h
 * @brief A voice in a call: the microphone made fit to send, the speaker
 * telling it what to take out, muting, and who is speaking.
 *
 * A VoiceChannel stands between one side's two devices with an
 * AudioProcessor: microphone() wraps the microphone, whose sound it cleans —
 * the speaker's echo taken out, noise suppressed, the level evened — and
 * speaker() wraps the speaker, so that what it plays, the other side, reaches
 * the processor as the echo to take out. Where there is no processor (a build
 * without one, or devices of another format than configured) the microphone's
 * sound goes as it is.
 *
 * Muted, the microphone sends silence, which the voice encoder's DTX makes
 * next to nothing; the stream stays, so unmuting is at once. speaking() is
 * this side's voice on the microphone, unmuted; hearing() is the other side
 * on the speaker. Each holds for a moment after the sound stops, so that a UI
 * showing them does not flicker. Thread-safe; the wrappers may outlive it.
 */

#include <memory>

#include "ratcast/client/audio_publisher.h"
#include "ratcast/client/call.h"
#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/audio_processing.h"
#include "ratcast/engine/audio_source.h"

namespace ratcast {

struct VoiceConfig {
    AudioFormat microphone{48'000, 1};
    AudioFormat speaker{48'000, 2};
    /// What the processor does; its formats are the two above.
    AudioProcessingConfig processing = {};
};

class VoiceChannel {
public:
    explicit VoiceChannel(VoiceConfig config = {}, Clock& clock = SteadyClock::instance());

    /// `device`'s sound, cleaned, and silent while muted.
    std::unique_ptr<AudioSource> microphone(std::unique_ptr<AudioSource> device);
    /// `device`, telling the processor what it plays.
    std::unique_ptr<AudioOutput> speaker(std::unique_ptr<AudioOutput> device);

    void set_muted(bool muted) noexcept;
    bool muted() const noexcept;
    bool speaking() const noexcept;
    bool hearing() const noexcept;
    /// Whether echo and noise are taken out: there is a processor.
    bool processed() const noexcept;

private:
    struct State;
    class Microphone;
    class Speaker;

    std::shared_ptr<State> state_;
};

/// Publish `microphone` — VoiceChannel::microphone()'s — on `call` as this
/// side's voice: a stream labelled kVoiceLabel, coded as speech with DTX and
/// FEC for some loss. Fails as Call::publish does.
Result<AudioPublisher*> publish_voice(Call& call, std::unique_ptr<AudioSource> microphone,
                                      StreamId stream = 4);

}  // namespace ratcast
