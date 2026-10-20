#pragma once

/**
 * @file audio_device.h
 * @brief Sound from the system's devices, and sound to them.
 *
 * Capture is a source like any other (AudioSource): a microphone, or what the
 * computer plays — the whole of it, or one application's sound. Playback is
 * the other way round: the device asks for sound when it needs some, and an
 * AudioOutput's Pull fills it in, knowing when that sound will be heard. That
 * is what a jitter buffer needs to play each frame at its time.
 *
 * Both ask the system for small device buffers (`latency`, 10 ms by default)
 * and convert to and from the format asked for: rate, channels and sample
 * format are the system's to match, never the caller's. On Linux they are
 * PipeWire streams, which need no permission beyond the session's.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/audio_frame.h"
#include "ratcast/engine/audio_source.h"

namespace ratcast {

// ── Capture ─────────────────────────────────────────────────────────────────

struct AudioCaptureOptions {
    AudioFormat format{48'000, 1};
    /// What to capture: 0 for the default microphone, or a device's or an
    /// application's node (on Linux a PipeWire node id), whose sound the
    /// capture ends with.
    uint32_t node = 0;
    /// With node 0: what the default output plays rather than a microphone.
    bool system_sound = false;
    Duration latency = std::chrono::milliseconds(10);
};

/// A capture as `options` say, ready to start. Fails with InvalidArgument for
/// a format with no channels or rate, or a latency outside 1–100 ms;
/// Unavailable where this system has no audio capture. A node that does not
/// exist ends the capture as it starts.
Result<std::unique_ptr<AudioSource>> open_audio_capture(const AudioCaptureOptions& options);

// ── Playback ────────────────────────────────────────────────────────────────

struct AudioOutputOptions {
    AudioFormat format{48'000, 2};
    /// Where to play: 0 for the default output, which it follows as that
    /// changes, or a device's node.
    uint32_t node = 0;
    Duration latency = std::chrono::milliseconds(10);
};

class AudioOutput {
public:
    /// Fill `out` — format().channels samples per sample time, interleaved —
    /// with the sound to be heard from `play_time` on, the local clock's time
    /// the first of them reaches the speaker. Zeros are silence. Called on the
    /// device's realtime thread: it must not block.
    using Pull = std::function<void(std::span<float> out, Timestamp play_time)>;

    /// How an output that stops by itself says so: once, with success when
    /// the device went and the failure otherwise.
    using EndHandler = std::function<void(Result<void> outcome)>;

    virtual ~AudioOutput() = default;

    virtual const AudioFormat& format() const noexcept = 0;

    /// Start asking `pull` for sound; `ended` (may be empty) hears when the
    /// output stops by itself. Neither may call stop(). Fails if the output
    /// cannot start or has already started.
    virtual Result<void> start(Pull pull, EndHandler ended) = 0;

    /// Stop. When this returns, neither handler is running or will be called
    /// again. The output may be started again.
    virtual void stop() = 0;
};

/// An output as `options` say, ready to start. Fails with InvalidArgument for
/// a format with no channels or rate, or a latency outside 1–100 ms;
/// Unavailable where this system has no audio output.
Result<std::unique_ptr<AudioOutput>> open_audio_output(const AudioOutputOptions& options);

}  // namespace ratcast
