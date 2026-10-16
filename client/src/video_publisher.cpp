#include "ratcast/client/video_publisher.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "librats/util/logger.h"

namespace ratcast {
namespace {

/// A source that has sent nothing for this long is taken to be showing a still
/// picture (a compositor sends only what changes).
constexpr auto kStillAfter = std::chrono::milliseconds(100);

}  // namespace

VideoPublisher::VideoPublisher(MediaSession& session, VideoPublisherConfig config, Clock& clock)
    : session_(session), config_(std::move(config)), clock_(clock) {}

VideoPublisher::~VideoPublisher() { stop(); }

Result<void> VideoPublisher::start(VideoSource& source, EndHandler ended) {
    std::lock_guard lifecycle(lifecycle_);
    if (source_) return fail(Errc::InvalidArgument);
    {
        std::lock_guard lock(mutex_);
        stopping_ = false;
        finished_ = false;
        keyframe_requested_ = false;
        have_picture_ = false;
    }
    ended_ = std::move(ended);
    thread_ = std::thread(&VideoPublisher::run, this);

    Result<void> started = source.start([this](VideoFrame picture) { take(std::move(picture)); },
                                        [this](Result<void> outcome) { finish(outcome); });
    if (!started) {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        thread_.join();
        return started;
    }
    source_ = &source;
    return {};
}

void VideoPublisher::stop() {
    std::lock_guard lifecycle(lifecycle_);
    if (!source_) return;
    source_->stop();  // no picture arrives after this
    source_ = nullptr;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        pending_.reset();
    }
    wake_.notify_one();
    thread_.join();

    if (announced_) session_.remove_stream(config_.stream);
    announced_.reset();
    encoder_.reset();
    last_ = VideoFrame{};
}

void VideoPublisher::request_keyframe() {
    {
        std::lock_guard lock(mutex_);
        keyframe_requested_ = true;
    }
    wake_.notify_one();
}

VideoPublisherStats VideoPublisher::stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

void VideoPublisher::take(VideoFrame picture) {
    bool replaced = false;
    {
        std::lock_guard lock(mutex_);
        replaced = pending_.has_value();
        pending_ = std::move(picture);
    }
    wake_.notify_one();
    std::lock_guard lock(stats_mutex_);
    ++stats_.pictures;
    if (replaced) ++stats_.skipped;
}

void VideoPublisher::finish(Result<void> outcome) {
    {
        std::lock_guard lock(mutex_);
        if (finished_) return;
        finished_ = true;
    }
    if (ended_) ended_(outcome);
}

void VideoPublisher::run() {
    auto last_taken = std::chrono::steady_clock::now();
    for (;;) {
        VideoFrame picture;
        bool keyframe = false;
        bool repeat = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] {
                return stopping_ || pending_ || (keyframe_requested_ && have_picture_);
            });
            if (!pending_ && !stopping_) {
                // A keyframe is wanted. A source that is not still brings a new
                // picture soon; only a still one has its last picture repeated.
                wake_.wait_until(lock, last_taken + kStillAfter,
                                 [&] { return stopping_ || pending_.has_value(); });
            }
            if (stopping_) return;
            last_taken = std::chrono::steady_clock::now();
            keyframe = std::exchange(keyframe_requested_, false);
            if (pending_) {
                picture = std::move(*pending_);
                pending_.reset();
            } else {
                picture = last_;  // a still screen: show the last picture again
                repeat = true;
            }
        }
        if (repeat) picture.set_capture_time(clock_.now());

        if (Result<void> published = publish(std::move(picture), keyframe); !published) {
            finish(published);
            return;
        }
        if (repeat) {
            std::lock_guard lock(stats_mutex_);
            ++stats_.repeated;
        }
    }
}

Result<void> VideoPublisher::publish(VideoFrame picture, bool keyframe) {
    if (!encoder_ || picture.width() != encoder_config_.width ||
        picture.height() != encoder_config_.height ||
        picture.color_space() != encoder_config_.color_space) {
        if (Result<void> opened = open_encoder(picture); !opened) return opened;
    }

    const auto start = std::chrono::steady_clock::now();
    const Result<void> encoded = encoder_->encode(
        picture, keyframe, [this](EncodedVideo&& frame) { send(std::move(frame)); });
    const auto took =
        std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - start);
    {
        std::lock_guard lock(stats_mutex_);
        stats_.encode_time += took;
        stats_.max_encode_time = std::max(stats_.max_encode_time, took);
        if (!encoded && stats_.errors++ == 0) {  // the first of what may be many
            LOG_WARN("client", "stream " << config_.stream << ": a picture failed to encode: "
                                         << to_string(encoded.error()));
        }
    }

    last_ = std::move(picture);
    std::lock_guard lock(mutex_);
    have_picture_ = true;
    return {};
}

Result<void> VideoPublisher::open_encoder(const VideoFrame& picture) {
    if (encoder_) {
        // What the old encoder still holds belongs before the new one's frames.
        (void)encoder_->flush([this](EncodedVideo&& frame) { send(std::move(frame)); });
        encoder_.reset();
    }

    VideoEncoderConfig config = config_.encoder;
    config.width = picture.width();
    config.height = picture.height();
    config.color_space = picture.color_space();
    Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config);
    if (!encoder) {
        LOG_WARN("client", "stream " << config_.stream << ": no encoder for " << config.width << "x"
                                     << config.height << ": " << to_string(encoder.error()));
        return fail(encoder.error());
    }
    encoder_ = std::move(*encoder);
    encoder_config_ = config;
    number_offset_ = next_number_;

    StreamDescription description;
    description.id = config_.stream;
    description.codec = config.codec;
    description.format =
        VideoFormat{config.width, config.height, config.frame_rate_num, config.frame_rate_den};
    description.lifetime = config_.lifetime;
    description.label = config_.label;
    const Result<void> announced =
        announced_ ? session_.update_stream(description) : session_.add_stream(description);
    if (!announced) return announced;
    announced_ = std::move(description);

    std::lock_guard lock(stats_mutex_);
    ++stats_.encoders;
    stats_.width = config.width;
    stats_.height = config.height;
    stats_.encoder = encoder_->info().name;
    return {};
}

void VideoPublisher::send(EncodedVideo&& frame) {
    // Each encoder numbers from 0; the stream's numbers carry on across them.
    FrameInfo info = frame.info;
    info.number += number_offset_;
    info.references.clear();
    for (FrameNumber reference : frame.info.references) {
        info.references.push_back(reference + number_offset_);
    }
    next_number_ = info.number + 1;

    const size_t size = frame.payload.size();
    if (Result<void> sent = session_.send_frame(config_.stream, info, std::move(frame.payload));
        !sent) {
        LOG_WARN("client", "stream " << config_.stream << ": frame " << info.number
                                     << " not sent: " << to_string(sent.error()));
        return;
    }
    std::lock_guard lock(stats_mutex_);
    ++stats_.frames;
    if (info.keyframe) ++stats_.keyframes;
    stats_.bytes += size;
}

}  // namespace ratcast
