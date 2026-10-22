#pragma once

/**
 * @file wasapi_audio.h
 * @brief Sound from WASAPI, and sound to it.
 *
 * Both are shared-mode streams of 32-bit float samples in the format asked
 * for, which the audio engine converts to and from the device's own (rate,
 * channels, sample format). Each runs on a thread of its own, woken by the
 * device, raised to the multimedia scheduler's "Pro Audio" class, and never
 * blocks there.
 *
 * Every stream is activated by the system's default device rather than by a
 * device of its own, so it follows the default as the user changes it, from
 * one headset to another, with no gap the caller sees. A capture of what
 * the computer plays — the whole of it, one process's, or all but one
 * process's — is a loopback, which hears nothing while nothing plays; the
 * capture fills that in with silence, so its blocks run on as a device's do.
 */

#include <cstdint>
#include <memory>
#include <mutex>

#include "ratcast/core/time.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/audio_source.h"

namespace ratcast::wasapi {

class AudioCapture final : public AudioSource {
public:
    /// Precondition: options passed open_audio_capture()'s checks. A node is
    /// a process id: that process's sound and its children's.
    explicit AudioCapture(const AudioCaptureOptions& options,
                          Clock& clock = SteadyClock::instance());
    ~AudioCapture() override;

    const AudioFormat& format() const noexcept override { return options_.format; }

    /// Fails with Unavailable when there is no such device, or the user has
    /// not let applications use the microphone. A process that is not there
    /// ends the capture with a failure; one that exits, with success.
    Result<void> start(Sink sink, EndHandler ended) override;
    void stop() override;

private:
    class Stream;  // the device side, one per start()

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

    /// Fails with Unavailable when there is no output, or for a node: on
    /// Windows the default output is the only one played to.
    Result<void> start(Pull pull, EndHandler ended) override;
    void stop() override;

private:
    class Stream;

    const AudioOutputOptions options_;
    Clock& clock_;
    std::mutex mutex_;
    std::unique_ptr<Stream> stream_;
};

}  // namespace ratcast::wasapi
