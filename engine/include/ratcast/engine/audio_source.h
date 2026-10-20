#pragma once

/**
 * @file audio_source.h
 * @brief Where sound comes from: a microphone, what the computer plays, a tone.
 *
 * A source pushes blocks of samples as it gets them, on a thread of its own —
 * for a device, its realtime thread. The sink must not block or the device
 * loses sound: it should hand the block to whatever encodes it and return.
 * Unlike pictures, no block may be dropped to catch up, so a sink keeps every
 * one; blocks are a few milliseconds each and an encoder takes them in
 * microseconds.
 *
 * Each block's capture time is that of its first sample, and blocks follow on
 * from one another: one's capture time plus its duration is about the next
 * one's.
 */

#include <functional>

#include "ratcast/core/result.h"
#include "ratcast/engine/audio_frame.h"

namespace ratcast {

class AudioSource {
public:
    using Sink = std::function<void(AudioFrame samples)>;

    /// How a source that stops by itself says so: once, from its thread, with
    /// success when there is nothing more to hear (the device went) and the
    /// failure otherwise.
    using EndHandler = std::function<void(Result<void> outcome)>;

    virtual ~AudioSource() = default;

    /// The format of the blocks it hands out.
    virtual const AudioFormat& format() const noexcept = 0;

    /// Start delivering blocks to `sink`; `ended` (may be empty) hears when the
    /// source stops by itself. Neither may call stop(). Fails if the source
    /// cannot start or has already started.
    virtual Result<void> start(Sink sink, EndHandler ended) = 0;

    /// Stop. When this returns, neither handler is running or will be called
    /// again. The source may be started again.
    virtual void stop() = 0;
};

}  // namespace ratcast
