#include "ratcast/client/data_player.h"

#include <utility>

#include "librats/util/logger.h"

namespace ratcast {

DataPlayer::DataPlayer(MediaSession& session, PeerId peer, StreamDescription description,
                       TsOutput& output, DataPlayerConfig config)
    : session_(session),
      peer_(std::move(peer)),
      description_(std::move(description)),
      output_(output),
      latency_(config.latency.value_or(description_.lifetime)),
      buffer_({.latency = latency_}) {}

DataPlayer::~DataPlayer() { stop(); }

Result<void> DataPlayer::start() {
    std::lock_guard lifecycle(lifecycle_);
    if (thread_.joinable() || description_.kind() != MediaKind::Data) {
        return fail(Errc::InvalidArgument);
    }
    {
        std::lock_guard lock(mutex_);
        buffer_.reset();
        running_ = true;
        finishing_ = false;
    }
    thread_ = std::thread(&DataPlayer::run, this);
    if (Result<void> subscribed = session_.subscribe(peer_, description_.id); !subscribed) {
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

void DataPlayer::stop() {
    std::lock_guard lifecycle(lifecycle_);
    if (!thread_.joinable()) return;
    session_.unsubscribe(peer_, description_.id);
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    wake_.notify_one();
    thread_.join();
}

void DataPlayer::push(EncodedFrame frame) {
    const Timestamp now = SteadyClock::instance().now();
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        buffer_.push(std::move(frame), now);
    }
    wake_.notify_one();
}

void DataPlayer::finish() {
    {
        std::lock_guard lock(mutex_);
        finishing_ = true;
    }
    wake_.notify_one();
}

DataPlayerStats DataPlayer::stats() const {
    std::lock_guard lock(mutex_);
    return {buffer_.stats(), bytes_, write_errors_, latency_};
}

// Write each frame as it falls due; sleep until the next one is, or a frame
// comes that may be due sooner.
void DataPlayer::run() {
    std::unique_lock lock(mutex_);
    while (running_) {
        if (std::optional<EncodedFrame> frame = buffer_.pop(SteadyClock::instance().now())) {
            lock.unlock();
            const Result<void> written = output_.write(frame->payload.bytes());
            lock.lock();
            if (written) {
                bytes_ += frame->payload.size();
            } else if (write_errors_++ == 0) {
                LOG_WARN("client", "stream " << description_.id << ": cannot write out: "
                                             << to_string(written.error()));
            }
            continue;
        }
        if (const std::optional<Timestamp> due = buffer_.next_due()) {
            wake_.wait_until(lock, *due);
        } else if (finishing_) {
            return;  // all written
        } else {
            wake_.wait(lock);
        }
    }
}

}  // namespace ratcast
