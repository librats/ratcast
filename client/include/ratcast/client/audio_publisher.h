#pragma once

/**
 * @file audio_publisher.h
 * @brief An audio source published as a stream: source → Opus → session.
 *
 * The publisher takes blocks from an AudioSource, encodes them on a thread of
 * its own — a device's thread must not wait on the network — and sends the
 * frames on a MediaSession stream, announced as it starts. Every block is
 * encoded, none skipped: sound that went missing on the sender's side would
 * be a gap no receiver could make up. A block is a few milliseconds and the
 * encoder takes microseconds, so the queue between them stays short; one that
 * grows past `max_queued` regardless (the thread was starved) loses its
 * oldest blocks.
 *
 * Sound has no keyframes to ask for and no rate control: an audio stream's
 * bitrate is a small, fixed share of the path.
 */

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/audio_codec.h"
#include "ratcast/engine/audio_frame.h"
#include "ratcast/engine/audio_source.h"
#include "ratcast/session/media_session.h"

namespace ratcast {

struct AudioPublisherConfig {
    StreamId stream = 2;
    std::string label = "sound";
    /// How long a frame is worth delivering: the stream's latency profile,
    /// and the most a receiver's jitter buffer waits.
    Duration lifetime = std::chrono::milliseconds(200);
    /// Content, bitrate, frame duration, DTX and FEC; the format comes from
    /// the source. By default a voice: silence sent as nothing, and FEC sized
    /// for some loss.
    AudioEncoderConfig encoder{.dtx = true, .expected_loss = 10};
    /// Blocks waiting for the encoder, at most.
    size_t max_queued = 64;
};

struct AudioPublisherStats {
    uint64_t blocks = 0;   ///< from the source
    uint64_t dropped = 0;  ///< blocks lost from a queue that grew too long
    uint64_t frames = 0;   ///< encoded and handed to the session
    uint64_t bytes = 0;    ///< of those frames
    uint64_t errors = 0;   ///< blocks the encoder failed on
    Duration encode_time{};
    std::string encoder;  ///< e.g. "libopus 1.5.2"
};

class AudioPublisher {
public:
    /// Once, when publishing stops by itself: with success when the source
    /// ran out, the failure otherwise.
    using EndHandler = std::function<void(Result<void> outcome)>;

    /// `session` must outlive the publisher.
    AudioPublisher(MediaSession& session, AudioPublisherConfig config);
    ~AudioPublisher();
    AudioPublisher(const AudioPublisher&) = delete;
    AudioPublisher& operator=(const AudioPublisher&) = delete;

    /// Announce the stream and publish what `source` produces until stop().
    /// The source must stay alive until then; `ended` (may be empty) must not
    /// call stop(). Fails if already started, for a format the encoder does
    /// not take, or with the session's or the source's own failure.
    Result<void> start(AudioSource& source, EndHandler ended = {});

    /// Stop the source and the encoder, and withdraw the stream.
    void stop();

    AudioPublisherStats stats() const;

private:
    void take(AudioFrame block);
    void finish(Result<void> outcome);
    void run();
    void send(EncodedAudio&& frame);

    MediaSession& session_;
    const AudioPublisherConfig config_;

    std::mutex lifecycle_;  ///< serialises start() and stop()
    AudioSource* source_ = nullptr;
    EndHandler ended_;
    std::thread thread_;
    std::unique_ptr<AudioEncoder> encoder_;  ///< the encoder thread's, while it runs

    // The hand-over from the source to the encoder thread.
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<AudioFrame> queue_;
    bool stopping_ = false;
    bool finished_ = false;

    mutable std::mutex stats_mutex_;
    AudioPublisherStats stats_;
};

}  // namespace ratcast
