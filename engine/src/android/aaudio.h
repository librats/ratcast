#pragma once

/**
 * @file aaudio.h
 * @brief Sound from AAudio, and sound to it (Android 8, API 26).
 *
 * Each is a low-latency stream of float samples in the format asked for,
 * which AAudio converts to and from the device's, run by AAudio's callback on
 * its realtime thread a burst at a time. Played sound is stamped by
 * AAudioStream_getTimestamp(): when a frame the stream counts reaches the
 * speaker, the pull's first one that far on from it; captured sound by when
 * its last frame came in the same way, the blocks run on by a SampleClock.
 *
 * A stream follows the default device as it changes — a headset plugged in,
 * Bluetooth connected — by opening it again when AAudio says its device went
 * (AAUDIO_ERROR_DISCONNECTED), on a thread of the stream's own, as nothing may
 * close a stream from its callbacks; one given a device (`device`, AAudio's
 * device id: Java's AudioDeviceInfo.getId()) ends when that device goes.
 *
 * Capture is of the microphone (RECORD_AUDIO, an application's to ask the
 * user for); what other applications play needs a MediaProjection, which
 * only Java reaches (android/playback_capture.h): SystemSound and nodes are
 * Unavailable.
 *
 * A call's streams (`call`) are voice communication's: the microphone through
 * the preset that has the platform cancel the echo of what the device plays
 * and suppress noise, the sound out as a call's, which the routing
 * enter_call() sets follows and the platform's canceller hears.
 */

#include <memory>
#include <mutex>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/audio_device.h"

namespace ratcast::aaudio {

/// The devices there are. AAudio lists none — Java's AudioManager does —
/// so this is empty: the default, followed as it changes, is what plays.
Result<std::vector<AudioDeviceInfo>> list_devices(bool input);

class AudioCapture final : public AudioSource {
public:
    /// Precondition: options passed open_audio_capture()'s checks.
    explicit AudioCapture(const AudioCaptureOptions& options,
                          Clock& clock = SteadyClock::instance());
    ~AudioCapture() override;

    const AudioFormat& format() const noexcept override { return options_.format; }

    /// Fails with Unavailable for anything but the microphone, or when the
    /// stream does not open (no microphone, no RECORD_AUDIO).
    Result<void> start(Sink sink, EndHandler ended) override;
    void stop() override;

private:
    class Stream;

    const AudioCaptureOptions options_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    std::unique_ptr<Stream> stream_;
};

class AudioPlayback final : public AudioOutput {
public:
    /// Precondition: options passed open_audio_output()'s checks.
    explicit AudioPlayback(const AudioOutputOptions& options,
                           Clock& clock = SteadyClock::instance());
    ~AudioPlayback() override;

    const AudioFormat& format() const noexcept override { return options_.format; }

    /// Fails with Unavailable when the stream does not open, or for a node:
    /// outputs are chosen by `device`.
    Result<void> start(Pull pull, EndHandler ended) override;
    void stop() override;

private:
    class Stream;

    const AudioOutputOptions options_;
    Clock& clock_;
    std::mutex mutex_;
    std::unique_ptr<Stream> stream_;
};

}  // namespace ratcast::aaudio
