#pragma once

/**
 * @file pipewire_audio.h
 * @brief Sound from a PipeWire node, and sound to one.
 *
 * Both are streams of 32-bit float samples in the format asked for, which
 * PipeWire's own converter matches to the device: rate, channels, sample
 * format. Both process on PipeWire's realtime data thread, asking for buffers
 * of `latency` (the node latency they tell the graph), and never block there.
 */

#include <cstdint>
#include <memory>
#include <mutex>

#include "ratcast/core/time.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/audio_source.h"

namespace ratcast::pipewire {

class AudioCapture final : public AudioSource {
public:
    /// Precondition: options passed open_audio_capture()'s checks.
    explicit AudioCapture(const AudioCaptureOptions& options,
                          Clock& clock = SteadyClock::instance());
    ~AudioCapture() override;

    const AudioFormat& format() const noexcept override { return options_.format; }

    /// Fails with Unavailable when PipeWire cannot be reached. A node that
    /// does not exist ends the capture with a failure.
    Result<void> start(Sink sink, EndHandler ended) override;
    void stop() override;

private:
    class Stream;  // the PipeWire side, one per start()

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

    /// Fails with Unavailable when PipeWire cannot be reached.
    Result<void> start(Pull pull, EndHandler ended) override;
    void stop() override;

    /// The playback's own node while it plays, 0 before PipeWire has made
    /// it: what a capture of this sound connects to.
    uint32_t node_id() const;

private:
    class Stream;

    const AudioOutputOptions options_;
    Clock& clock_;
    mutable std::mutex mutex_;
    std::unique_ptr<Stream> stream_;
};

}  // namespace ratcast::pipewire
