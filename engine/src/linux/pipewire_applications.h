#pragma once

/**
 * @file pipewire_applications.h
 * @brief What the applications play, but one process's: a PipeWire filter
 *        that links itself to every application's output.
 *
 * The default output's monitor is all or nothing — a call's own voices too.
 * So this is a node of its own, a filter with two input ports, left and right,
 * and the links to it are made here: from each output port of every node
 * playing sound ("Stream/Output/Audio") whose client is not the excluded
 * process, by the channel the port carries — a left one to the left, a right
 * one to the right, a centre or a mono one to both, the LFE to neither — and
 * PipeWire adds up what comes in on a port. A mono capture is the two sides'
 * mean. Clients, nodes and ports are followed in the registry as they come
 * and go; a link goes with its port, and all of them with the filter. The
 * filter is processed with nothing linked too, so the sound runs on, silent,
 * between applications.
 *
 * Unlike a stream it has no converter: its samples are the graph's, at the
 * graph's rate. It asks the graph for the format's rate; a graph that runs at
 * another ends the capture with Unsupported.
 */

#include <memory>
#include <mutex>

#include "ratcast/core/time.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/audio_source.h"

namespace ratcast::pipewire {

class ApplicationsCapture final : public AudioSource {
public:
    /// Precondition: options passed open_audio_capture()'s checks.
    explicit ApplicationsCapture(const AudioCaptureOptions& options,
                                 Clock& clock = SteadyClock::instance());
    ~ApplicationsCapture() override;

    const AudioFormat& format() const noexcept override { return options_.format; }

    /// Fails with Unavailable when PipeWire cannot be reached.
    Result<void> start(Sink sink, EndHandler ended) override;
    void stop() override;

private:
    class Graph;  // the PipeWire side, one per start()

    const AudioCaptureOptions options_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    std::unique_ptr<Graph> graph_;
};

}  // namespace ratcast::pipewire
