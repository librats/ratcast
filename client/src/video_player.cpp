#include "ratcast/client/video_player.h"

#include <chrono>
#include <utility>

#include "librats/util/logger.h"

namespace ratcast {

VideoPlayer::VideoPlayer(MediaSession& session, PeerId peer, StreamDescription description,
                         Sink sink, VideoPlayerConfig config)
    : session_(session),
      peer_(std::move(peer)),
      stream_(description.id),
      sink_(std::move(sink)),
      config_(config),
      description_(std::move(description)),
      schedule_(max_delay()) {}

VideoPlayer::~VideoPlayer() { stop(); }

Result<void> VideoPlayer::start() {
    std::lock_guard lifecycle(lifecycle_);
    if (thread_.joinable()) return fail(Errc::InvalidArgument);
    if (Result<void> opened = open_decoder(description_); !opened) return opened;
    {
        std::lock_guard lock(mutex_);
        running_ = true;
        awaiting_keyframe_ = false;
        queue_.clear();
    }
    schedule_.reset();
    held_.clear();
    decode_estimate_ = Duration::zero();
    thread_ = std::thread(&VideoPlayer::run, this);
    if (Result<void> subscribed = session_.subscribe(peer_, stream_); !subscribed) {
        {
            std::lock_guard lock(mutex_);
            running_ = false;
        }
        wake_.notify_one();
        thread_.join();
        return subscribed;
    }
    return {};
}

void VideoPlayer::stop() {
    std::lock_guard lifecycle(lifecycle_);
    if (!thread_.joinable()) return;
    session_.unsubscribe(peer_, stream_);
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        queue_.clear();
    }
    wake_.notify_one();
    thread_.join();
    decoder_.reset();
    held_.clear();
}

void VideoPlayer::push(EncodedFrame frame) {
    const Timestamp now = SteadyClock::instance().now();
    bool overflow = false;
    size_t dropped = 0;
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        if (queue_.size() >= config_.max_queued_frames) {
            // The decoder cannot keep up: start again from a keyframe rather
            // than lag further behind.
            dropped = queue_.size() + 1;
            queue_.clear();
            awaiting_keyframe_ = true;
            overflow = true;
        } else {
            queue_.push_back({std::move(frame), now});
        }
    }
    {
        std::lock_guard lock(stats_mutex_);
        ++stats_.frames;
        stats_.dropped += dropped;
    }
    if (overflow) {
        request_keyframe();
    } else {
        wake_.notify_one();
    }
}

void VideoPlayer::update(const StreamDescription& description) {
    std::lock_guard lock(mutex_);
    new_description_ = description;
}

VideoPlayerStats VideoPlayer::stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

void VideoPlayer::run() {
    for (;;) {
        std::optional<Arrival> arrival;
        std::optional<StreamDescription> description;
        bool dropped = false;
        {
            std::unique_lock lock(mutex_);
            // Wake for a frame to decode, or when the next picture is due.
            const auto woken = [&] { return !running_ || !queue_.empty(); };
            if (held_.empty()) {
                wake_.wait(lock, woken);
            } else {
                wake_.wait_until(lock, held_.front().due, woken);
            }
            if (!running_) return;
            if (!queue_.empty()) {
                arrival = std::move(queue_.front());
                queue_.pop_front();
                dropped = awaiting_keyframe_ && !arrival->frame.info.keyframe;
                if (!dropped) {
                    awaiting_keyframe_ = false;
                    description = std::exchange(new_description_, std::nullopt);
                }
            }
        }

        // Another codec needs another decoder; a new size is the decoder's to
        // follow, from the keyframe that brings it.
        if (description) {
            const bool other_codec = description->codec != description_.codec;
            description_ = std::move(*description);
            if (other_codec && !open_decoder(description_)) decoder_.reset();
            schedule_.set_max_delay(max_delay());
        }
        if (arrival && (dropped || !decoder_)) {  // !decoder_: a codec this build cannot decode
            std::lock_guard stats(stats_mutex_);
            ++stats_.dropped;
        } else if (arrival) {
            decode(*arrival);
        }
        show_due();
    }
}

void VideoPlayer::decode(const Arrival& arrival) {
    const EncodedFrame& frame = arrival.frame;
    schedule_.on_frame(frame.info.capture_time, arrival.at, frame.info.keyframe);
    const auto start = std::chrono::steady_clock::now();
    const Result<void> decoded = decoder_->decode(frame, [this](VideoFrame&& picture) {
        Timestamp due = schedule_.due(picture.capture_time());
        if (config_.sync) due = config_.sync->due(picture.capture_time(), due, max_delay());
        held_.push_back({due, std::move(picture)});
    });
    const auto took =
        std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - start);
    decode_estimate_ += (took - decode_estimate_) / 8;
    if (config_.sync) config_.sync->pictures_need(schedule_.offset() + decode_estimate_);
    {
        std::lock_guard lock(stats_mutex_);
        stats_.decode_time += took;
        stats_.max_decode_time = std::max(stats_.max_decode_time, took);
        if (!decoded) ++stats_.decode_errors;
    }
    if (!decoded) {
        LOG_DEBUG("client", "stream " << stream_ << ": frame " << frame.info.number
                                      << " failed to decode: " << to_string(decoded.error()));
        {
            std::lock_guard lock(mutex_);
            awaiting_keyframe_ = true;
        }
        request_keyframe();
    }
}

// Hand on the pictures whose time has come, in order.
void VideoPlayer::show_due() {
    const Timestamp now = SteadyClock::instance().now();
    while (!held_.empty() && held_.front().due <= now) {
        VideoFrame picture = std::move(held_.front().picture);
        held_.pop_front();
        const Timestamp capture = picture.capture_time();
        {
            std::lock_guard lock(stats_mutex_);
            ++stats_.pictures;
            stats_.delay = schedule_.delay();
            stats_.width = picture.width();
            stats_.height = picture.height();
        }
        sink_(std::move(picture));
        if (config_.sync) config_.sync->picture_shown(capture);
    }
}

Duration VideoPlayer::max_delay() const noexcept {
    return config_.max_delay.value_or(description_.lifetime);
}

Result<void> VideoPlayer::open_decoder(const StreamDescription& description) {
    if (media_kind_of(description.codec) != MediaKind::Video) return fail(Errc::InvalidArgument);
    Result<std::unique_ptr<VideoDecoder>> decoder =
        open_video_decoder({description.codec, description.codec_config});
    if (!decoder) {
        LOG_WARN("client",
                 "stream " << description.id << ": no decoder: " << to_string(decoder.error()));
        return fail(decoder.error());
    }
    decoder_ = std::move(*decoder);
    std::lock_guard lock(stats_mutex_);
    stats_.decoder = decoder_->info().name;
    return {};
}

void VideoPlayer::request_keyframe() {
    session_.request_keyframe(peer_, stream_);
    std::lock_guard lock(stats_mutex_);
    ++stats_.keyframe_requests;
}

}  // namespace ratcast
