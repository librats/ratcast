#pragma once

/**
 * @file core_audio.h
 * @brief Sound from Core Audio, and sound to it.
 *
 * Capture is an IOProc on a device: a microphone's, or — for what the
 * computer plays — a private aggregate device made of a process tap (macOS
 * 14.2): a global tap of every process's sound, all but one process's, or one
 * process's own, or a tap of what one output plays. The device's float
 * samples are mapped to the channels asked for and converted to the rate
 * asked for (an AudioConverter), each block stamped by the device's host time
 * less its latency. A tap runs on in silence while nothing plays.
 *
 * Playback is an output unit pulled on the device's realtime thread: the
 * default output's own, which follows the default as the user changes it, or
 * AUHAL on one device. The unit converts from the format asked for.
 *
 * Either follows the default device as it changes — the capture by opening
 * the new one — unless it is given a device (`device`, a device's UID), which
 * it keeps to while it is there, ending when it goes. Changes are handled on
 * a serial queue of the stream's own, which stop() empties before it returns.
 */

#include <memory>
#include <mutex>
#include <vector>

#include "ratcast/core/time.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/audio_source.h"

namespace ratcast::core_audio {

/// The devices with inputs, or those with outputs, by their UIDs.
Result<std::vector<AudioDeviceInfo>> list_devices(bool input);

class AudioCapture final : public AudioSource {
public:
    /// Precondition: options passed open_audio_capture()'s checks. A node is
    /// a process id: that process's sound.
    explicit AudioCapture(const AudioCaptureOptions& options,
                          Clock& clock = SteadyClock::instance());
    ~AudioCapture() override;

    const AudioFormat& format() const noexcept override { return options_.format; }

    /// Fails with Unavailable when there is no such device, or no process
    /// taps (before macOS 14.2) for what the computer plays. A process that
    /// is not there ends the capture with a failure; one that exits, with
    /// success. Without the user's leave the system hands out silence.
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
    /// macOS outputs are chosen by `device`.
    Result<void> start(Pull pull, EndHandler ended) override;
    void stop() override;

private:
    class Stream;

    const AudioOutputOptions options_;
    Clock& clock_;
    std::mutex mutex_;
    std::unique_ptr<Stream> stream_;
};

}  // namespace ratcast::core_audio
