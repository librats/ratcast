#pragma once

/**
 * @file audio_player.h
 * @brief A subscribed audio stream played back: session → jitter buffer →
 * decoder → device.
 *
 * Frames arrive on a network thread as they come (MediaSession's
 * frame_received) and go into a JitterBuffer. The output pulls sound when it
 * needs some, on its own thread; the player asks the jitter buffer what comes
 * next — a frame, or a gap — and decodes it there and then: a frame takes the
 * decoder some tens of microseconds, and only that thread touches it. A gap is
 * made up by the decoder: from the next frame's FEC where a frame was lost,
 * concealment otherwise.
 *
 * Each frame's capture time and the time it will be heard go to an optional
 * observer, on the output's thread: what latency and, later, lip sync are
 * measured with.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/audio_codec.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/audio_frame.h"
#include "ratcast/session/media_session.h"
#include "ratcast/stream/frame.h"
#include "ratcast/stream/jitter_buffer.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast {

struct AudioPlayerConfig {
    /// The most the jitter buffer waits for a frame. nullopt: the stream's
    /// lifetime, past which no frame comes.
    std::optional<Duration> max_delay;
};

struct AudioPlayerStats {
    uint64_t frames = 0;         ///< from the session
    JitterBuffer::Stats jitter;  ///< what was played, and how
    AudioDecoderStats decoded;   ///< sound decoded, recovered by FEC, made up
    uint64_t decode_errors = 0;
    Duration delay{};     ///< the jitter buffer's, beyond the fastest frames
    Duration target{};    ///< what it aims at
    std::string decoder;  ///< e.g. "libopus 1.5.2"
};

class AudioPlayer {
public:
    /// A frame goes to the device: captured at `capture_time` by the sender's
    /// clock, heard at `play_time` by the output's. Called on the output's
    /// thread; must not block.
    using Observer = std::function<void(Timestamp capture_time, Timestamp play_time)>;

    /// Plays `peer`'s stream `description.id` on `session` into `output`; both
    /// must outlive the player, and `output` is the player's alone while it
    /// plays. `clock` is the output's: what play times are on.
    AudioPlayer(MediaSession& session, PeerId peer, StreamDescription description,
                AudioOutput& output, AudioPlayerConfig config = {}, Observer observer = {},
                Clock& clock = SteadyClock::instance());
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    /// Open a decoder, start the output and subscribe. Fails with Unavailable
    /// when there is no decoder for the stream's codec, InvalidArgument if
    /// already started or the session refuses the subscription, or with the
    /// output's own failure.
    Result<void> start();

    /// Unsubscribe and stop the output; the observer is not called once this
    /// returns.
    void stop();

    /// A frame of the stream, from MediaSession's frame_received. Never blocks
    /// for long.
    void push(EncodedFrame frame);

    const PeerId& peer() const noexcept { return peer_; }
    StreamId stream() const noexcept { return description_.id; }
    AudioPlayerStats stats() const;

private:
    void pull(std::span<float> out, Timestamp play_time);
    AudioFrame render(const JitterBuffer::Next& next);

    MediaSession& session_;
    const PeerId peer_;
    const StreamDescription description_;
    AudioOutput& output_;
    const Observer observer_;
    Clock& clock_;

    std::mutex lifecycle_;  ///< serialises start() and stop()
    bool started_ = false;

    // The output thread's own (and start()'s, before the output runs).
    std::unique_ptr<AudioDecoder> decoder_;
    AudioFrame pending_;         ///< decoded, not all of it played yet
    size_t pending_played_ = 0;  ///< of pending_, samples per channel

    mutable std::mutex mutex_;  ///< the jitter buffer, between network and output
    JitterBuffer jitter_;
    AudioPlayerStats stats_;
};

}  // namespace ratcast
