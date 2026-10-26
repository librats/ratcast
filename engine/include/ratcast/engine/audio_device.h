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
 * PipeWire streams, which need no permission beyond the session's; on Windows
 * WASAPI streams, which need the user's leave for the microphone. Either
 * follows the default device as the user changes it, or plays to, or hears,
 * one device the user chose from those listed (`device`): by an id that lasts
 * from one run to the next, the default in its place while it is not there.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/audio_frame.h"
#include "ratcast/engine/audio_source.h"

namespace ratcast {

// ── Devices ─────────────────────────────────────────────────────────────────

/// A sound device, as the system lists it.
struct AudioDeviceInfo {
    /// What `device` takes: the same from one run to the next (on Linux a
    /// PipeWire node's name; on Windows an endpoint's id).
    std::string id;
    std::string name;  ///< as the system names it, for a UI
};

/// The inputs there are now — microphones, and anything else the system
/// lists as one — and the outputs. Fail with Unavailable where this system
/// has no way to reach them; none there is an empty list.
Result<std::vector<AudioDeviceInfo>> list_audio_inputs();
Result<std::vector<AudioDeviceInfo>> list_audio_outputs();

// ── Capture ─────────────────────────────────────────────────────────────────

/// What a capture of no particular node hears.
enum class AudioCaptureSource : uint8_t {
    Microphone,   ///< the default input
    SystemSound,  ///< what an output plays, this process's sound too
    /// What the applications play — each one's sound, added up as the output
    /// would — other than one process's: what to share beside a screen while
    /// a call's voices play, which must not go back out. Applications that
    /// start playing later are heard as they start; it runs on in silence
    /// while none plays.
    Applications,
};

struct AudioCaptureOptions {
    AudioFormat format{48'000, 1};
    /// What to capture: 0 for `source`, or a device's or an application's
    /// node (on Linux a PipeWire node id; on Windows a process id: its sound
    /// and its children's), whose sound the capture ends with.
    uint32_t node = 0;
    AudioCaptureSource source = AudioCaptureSource::Microphone;
    /// With Microphone, the input of this id (AudioDeviceInfo::id); with
    /// SystemSound, what the output of this id plays. "" for the default,
    /// followed as it changes; not there, the default is heard.
    std::string device = {};
    /// With Applications: the process whose sound is left out; 0 for this one.
    int excluded_process = 0;
    Duration latency = std::chrono::milliseconds(10);
};

/// A capture as `options` say, ready to start. Fails with InvalidArgument for
/// a format with no channels or rate, a latency outside 1–100 ms, or
/// Applications in more than two channels; Unavailable where this system has
/// no audio capture. A node that does not exist ends the capture as it starts.
Result<std::unique_ptr<AudioSource>> open_audio_capture(const AudioCaptureOptions& options);

// ── Playback ────────────────────────────────────────────────────────────────

struct AudioOutputOptions {
    AudioFormat format{48'000, 2};
    /// Where to play: 0 for `device`, or a device's node (on Linux; Windows
    /// fails to start with one).
    uint32_t node = 0;
    /// The output of this id (AudioDeviceInfo::id); "" for the default,
    /// followed as it changes. Not there, it plays to the default.
    std::string device = {};
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
