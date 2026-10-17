#pragma once

/**
 * @file video_publisher.h
 * @brief A video source published as a stream: source → encoder → session.
 *
 * The publisher takes pictures from a VideoSource, encodes them on a thread of
 * its own and sends the frames on a MediaSession stream, which it announces
 * with the first picture and announces again when the picture's size changes.
 *
 * It keeps only the newest picture: when the encoder falls behind, older ones
 * are skipped, never queued, so latency does not build up. It also keeps the
 * last picture it encoded, because a screen that does not change sends
 * nothing: a viewer who joins, or asks for a keyframe to recover, still gets
 * one — the last picture again.
 *
 * A change of size or colour space starts a new encoder, with a keyframe;
 * frame numbers carry on, so receivers see one stream throughout.
 *
 * The stream is rate-controlled unless configured otherwise: the session
 * follows the paths to the viewers and the publisher hands the bitrate it
 * asks for (set_bitrate()) to the encoder before the next picture.
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/video_codec.h"
#include "ratcast/engine/video_frame.h"
#include "ratcast/engine/video_source.h"
#include "ratcast/session/media_session.h"
#include "ratcast/stream/stream_description.h"

namespace ratcast {

struct VideoPublisherConfig {
    StreamId stream = 1;
    std::string label = "screen";
    /// How long a frame is worth delivering: the stream's latency profile.
    Duration lifetime = std::chrono::milliseconds(200);
    /// Codec, bitrate, frame rate and encoder choice. Size and colour space
    /// come from the pictures. The bitrate is the most rate control asks for.
    VideoEncoderConfig encoder;
    /// Rate control's start, low so that the first keyframe is small, and its
    /// floor; both capped at encoder.bitrate. Without rate control the
    /// encoder keeps encoder.bitrate.
    bool rate_control = true;
    uint32_t start_bitrate = 2'000'000;
    uint32_t min_bitrate = 250'000;
};

struct VideoPublisherStats {
    uint64_t pictures = 0;  ///< from the source
    uint64_t skipped = 0;   ///< replaced by a newer picture before the encoder took them
    uint64_t repeated = 0;  ///< the last picture encoded again for a keyframe
    uint64_t frames = 0;    ///< encoded and handed to the session
    uint64_t keyframes = 0;
    uint64_t bytes = 0;           ///< of those frames
    uint64_t keyframe_bytes = 0;  ///< of the keyframes among them
    uint64_t encoders = 0;        ///< opened: one, and one more per change of size or colours
    uint64_t errors = 0;          ///< pictures the encoder failed on
    Duration encode_time{};
    Duration max_encode_time{};
    uint32_t width = 0;  ///< of the pictures now
    uint32_t height = 0;
    uint32_t bitrate = 0;  ///< what the encoder aims at now
    std::string encoder;   ///< the encoder in use, e.g. "libsvtav1"
};

class VideoPublisher {
public:
    /// Once, when publishing stops by itself: with success when the source
    /// ran out, the failure otherwise (the encoder could not be opened).
    using EndHandler = std::function<void(Result<void> outcome)>;

    /// `session` and `clock` must outlive the publisher.
    explicit VideoPublisher(MediaSession& session, VideoPublisherConfig config,
                            Clock& clock = SteadyClock::instance());
    ~VideoPublisher();
    VideoPublisher(const VideoPublisher&) = delete;
    VideoPublisher& operator=(const VideoPublisher&) = delete;

    /// Publish what `source` produces until stop(). The source must stay
    /// alive until then; `ended` (may be empty) must not call stop(). Fails if
    /// already started, or with the source's own failure.
    Result<void> start(VideoSource& source, EndHandler ended = {});

    /// Stop the source and the encoder, and withdraw the stream.
    void stop();

    /// Make the next frame a keyframe: what MediaSession's keyframe_needed
    /// asks for. Any thread.
    void request_keyframe();

    /// Aim at `bits_per_second` from the next picture on: what MediaSession's
    /// bitrate_changed asks for. Any thread.
    void set_bitrate(uint32_t bits_per_second);

    VideoPublisherStats stats() const;

private:
    void take(VideoFrame picture);
    void finish(Result<void> outcome);
    void run();
    Result<void> publish(VideoFrame picture, bool keyframe);
    Result<void> open_encoder(const VideoFrame& picture);
    std::optional<BitrateLimits> bitrate_limits() const;
    void send(EncodedVideo&& frame);

    MediaSession& session_;
    const VideoPublisherConfig config_;
    Clock& clock_;

    std::mutex lifecycle_;  ///< serialises start() and stop()
    VideoSource* source_ = nullptr;
    EndHandler ended_;
    std::thread thread_;

    // The hand-over from the source to the encoder thread.
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<VideoFrame> pending_;
    bool keyframe_requested_ = false;
    bool have_picture_ = false;  ///< the encoder thread has a last picture to repeat
    bool stopping_ = false;
    bool finished_ = false;
    std::atomic<uint32_t> bitrate_{0};  ///< the latest set_bitrate(), or where start() began

    // The encoder thread's own.
    std::unique_ptr<VideoEncoder> encoder_;
    VideoEncoderConfig encoder_config_;  ///< what encoder_ was opened with
    VideoFrame last_;
    Timestamp last_encoded_at_{};  ///< on clock_, when last_ went to the encoder
    std::optional<StreamDescription> announced_;
    FrameNumber next_number_ = 0;
    FrameNumber number_offset_ = 0;  ///< added to the current encoder's frame numbers

    mutable std::mutex stats_mutex_;
    VideoPublisherStats stats_;
};

}  // namespace ratcast
