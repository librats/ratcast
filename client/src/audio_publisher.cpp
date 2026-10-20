#include "ratcast/client/audio_publisher.h"

#include <chrono>
#include <utility>

#include "librats/util/logger.h"

namespace ratcast {

AudioPublisher::AudioPublisher(MediaSession& session, AudioPublisherConfig config)
    : session_(session), config_(std::move(config)) {}

AudioPublisher::~AudioPublisher() { stop(); }

Result<void> AudioPublisher::start(AudioSource& source, EndHandler ended) {
    std::lock_guard lifecycle(lifecycle_);
    if (source_) return fail(Errc::InvalidArgument);

    AudioEncoderConfig encoder_config = config_.encoder;
    encoder_config.format = source.format();
    Result<std::unique_ptr<AudioEncoder>> encoder = open_audio_encoder(encoder_config);
    if (!encoder) {
        LOG_WARN("client", "stream " << config_.stream
                                     << ": no audio encoder: " << to_string(encoder.error()));
        return fail(encoder.error());
    }
    StreamDescription description;
    description.id = config_.stream;
    description.codec = encoder_config.codec;
    description.format = encoder_config.format;
    description.lifetime = config_.lifetime;
    description.label = config_.label;
    if (Result<void> added = session_.add_stream(description); !added) return added;

    encoder_ = std::move(*encoder);
    {
        std::lock_guard lock(mutex_);
        stopping_ = false;
        finished_ = false;
        queue_.clear();
    }
    {
        std::lock_guard lock(stats_mutex_);
        stats_.encoder = encoder_->info().name;
    }
    ended_ = std::move(ended);
    thread_ = std::thread(&AudioPublisher::run, this);

    Result<void> started = source.start([this](AudioFrame block) { take(std::move(block)); },
                                        [this](Result<void> outcome) { finish(outcome); });
    if (!started) {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        thread_.join();
        encoder_.reset();
        session_.remove_stream(config_.stream);
        return started;
    }
    source_ = &source;
    return {};
}

void AudioPublisher::stop() {
    std::lock_guard lifecycle(lifecycle_);
    if (!source_) return;
    source_->stop();  // no block arrives after this
    source_ = nullptr;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    wake_.notify_one();
    thread_.join();
    encoder_.reset();
    session_.remove_stream(config_.stream);
}

AudioPublisherStats AudioPublisher::stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

void AudioPublisher::take(AudioFrame block) {
    bool dropped = false;
    {
        std::lock_guard lock(mutex_);
        if (queue_.size() >= config_.max_queued) {
            queue_.pop_front();
            dropped = true;
        }
        queue_.push_back(std::move(block));
    }
    wake_.notify_one();
    std::lock_guard lock(stats_mutex_);
    ++stats_.blocks;
    if (dropped) ++stats_.dropped;
}

void AudioPublisher::finish(Result<void> outcome) {
    {
        std::lock_guard lock(mutex_);
        if (finished_) return;
        finished_ = true;
    }
    if (ended_) ended_(outcome);
}

void AudioPublisher::run() {
    std::deque<AudioFrame> blocks;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            blocks.swap(queue_);  // all there is, in one go
        }
        for (const AudioFrame& block : blocks) {
            const auto start = std::chrono::steady_clock::now();
            const Result<void> encoded =
                encoder_->encode(block, [this](EncodedAudio&& frame) { send(std::move(frame)); });
            const auto took =
                std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - start);
            std::lock_guard lock(stats_mutex_);
            stats_.encode_time += took;
            if (!encoded && stats_.errors++ == 0) {  // the first of what may be many
                LOG_WARN("client", "stream " << config_.stream << ": a block failed to encode: "
                                             << to_string(encoded.error()));
            }
        }
        blocks.clear();
    }
}

void AudioPublisher::send(EncodedAudio&& frame) {
    const size_t size = frame.payload.size();
    if (Result<void> sent =
            session_.send_frame(config_.stream, frame.info, std::move(frame.payload));
        !sent) {
        LOG_WARN("client", "stream " << config_.stream << ": frame " << frame.info.number
                                     << " not sent: " << to_string(sent.error()));
        return;
    }
    std::lock_guard lock(stats_mutex_);
    ++stats_.frames;
    stats_.bytes += size;
}

}  // namespace ratcast
