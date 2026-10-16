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
      description_(std::move(description)) {}

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
}

void VideoPlayer::push(EncodedFrame frame) {
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
            queue_.push_back(std::move(frame));
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
        EncodedFrame frame;
        std::optional<StreamDescription> description;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return !running_ || !queue_.empty(); });
            if (!running_) return;
            frame = std::move(queue_.front());
            queue_.pop_front();
            if (awaiting_keyframe_ && !frame.info.keyframe) {
                lock.unlock();
                std::lock_guard stats(stats_mutex_);
                ++stats_.dropped;
                continue;
            }
            awaiting_keyframe_ = false;
            description = std::exchange(new_description_, std::nullopt);
        }

        // Another codec needs another decoder; a new size is the decoder's to
        // follow, from the keyframe that brings it.
        if (description) {
            const bool other_codec = description->codec != description_.codec;
            description_ = std::move(*description);
            if (other_codec && !open_decoder(description_)) decoder_.reset();
        }
        if (!decoder_) {  // a codec this build cannot decode
            std::lock_guard stats(stats_mutex_);
            ++stats_.dropped;
            continue;
        }
        decode(frame);
    }
}

void VideoPlayer::decode(const EncodedFrame& frame) {
    const auto start = std::chrono::steady_clock::now();
    const Result<void> decoded = decoder_->decode(frame, [this](VideoFrame&& picture) {
        {
            std::lock_guard lock(stats_mutex_);
            ++stats_.pictures;
            stats_.width = picture.width();
            stats_.height = picture.height();
        }
        sink_(std::move(picture));
    });
    const auto took =
        std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - start);
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
