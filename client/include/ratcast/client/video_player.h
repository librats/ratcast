#pragma once

/**
 * @file video_player.h
 * @brief A subscribed stream played back: session → decoder → pictures.
 *
 * Frames arrive on a network thread, in decode order (MediaSession's
 * frame_received); the player queues them for a decoder thread of its own and
 * hands the pictures on from there, each when PlayoutSchedule says: a frame
 * that came late — sent again on a lossy path — holds up the ones behind it,
 * and pictures held back as long play on evenly past it. When decoding falls
 * behind or fails, the player drops what it holds, asks the sender for a
 * keyframe and carries on from it: the picture stands still for a moment
 * rather than lagging for good.
 */

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "ratcast/client/playout_schedule.h"
#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/video_codec.h"
#include "ratcast/engine/video_frame.h"
#include "ratcast/session/media_session.h"
#include "ratcast/stream/frame.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast {

struct VideoPlayerConfig {
    /// Frames waiting for the decoder before the player gives up on them and
    /// starts again from a keyframe.
    size_t max_queued_frames = 16;
    /// The most a picture is held back to play evenly past late frames.
    /// nullopt: the stream's lifetime, past which no frame comes; zero: every
    /// picture as soon as it is decoded.
    std::optional<Duration> max_delay;
};

struct VideoPlayerStats {
    uint64_t frames = 0;    ///< from the session
    uint64_t pictures = 0;  ///< decoded and handed on
    uint64_t dropped = 0;   ///< frames thrown away to start again from a keyframe
    uint64_t decode_errors = 0;
    uint64_t keyframe_requests = 0;
    Duration decode_time{};
    Duration max_decode_time{};
    Duration delay{};    ///< pictures are held back for, past the fastest frames
    uint32_t width = 0;  ///< of the last picture
    uint32_t height = 0;
    std::string decoder;  ///< the decoder in use, e.g. "libdav1d"
};

class VideoPlayer {
public:
    /// Called on the player's thread with each picture, in order.
    using Sink = std::function<void(VideoFrame picture)>;

    /// Plays `peer`'s stream `description.id` on `session`, which must outlive
    /// the player.
    VideoPlayer(MediaSession& session, PeerId peer, StreamDescription description, Sink sink,
                VideoPlayerConfig config = {});
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    /// Open a decoder and subscribe. Fails with Unavailable when there is no
    /// decoder for the stream's codec, InvalidArgument if already started or
    /// the session refuses the subscription.
    Result<void> start();

    /// Unsubscribe and stop decoding; the sink is not called once this returns.
    void stop();

    /// A frame of the stream, from MediaSession's frame_received. Never blocks.
    void push(EncodedFrame frame);

    /// The sender announced the stream again: a new size, or another codec.
    void update(const StreamDescription& description);

    const PeerId& peer() const noexcept { return peer_; }
    StreamId stream() const noexcept { return stream_; }
    VideoPlayerStats stats() const;

private:
    struct Arrival {
        EncodedFrame frame;
        Timestamp at;
    };
    struct Held {
        Timestamp due;
        VideoFrame picture;
    };

    void run();
    void decode(const Arrival& arrival);
    void show_due();
    Duration max_delay() const noexcept;
    Result<void> open_decoder(const StreamDescription& description);
    void request_keyframe();

    MediaSession& session_;
    const PeerId peer_;
    const StreamId stream_;
    const Sink sink_;
    const VideoPlayerConfig config_;

    std::mutex lifecycle_;  ///< serialises start() and stop()
    std::thread thread_;

    // The hand-over from the network thread to the decoder thread.
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Arrival> queue_;
    std::optional<StreamDescription> new_description_;  ///< for the decoder thread to pick up
    bool awaiting_keyframe_ = false;                    ///< drop frames until one
    bool running_ = false;

    // The decoder thread's own (and start()'s, before the thread runs).
    StreamDescription description_;
    std::unique_ptr<VideoDecoder> decoder_;
    PlayoutSchedule schedule_;
    std::deque<Held> held_;  ///< decoded, until due

    mutable std::mutex stats_mutex_;
    VideoPlayerStats stats_;
};

}  // namespace ratcast
