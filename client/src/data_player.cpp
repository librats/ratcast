#include "ratcast/client/data_player.h"

#include <utility>
#include <vector>

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
    bool sooner = false;
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        buffer_.push(std::move(frame), now);
        // Most frames fall due after the one the writer waits for, which wakes
        // it anyway; only one due sooner — the first, or a repair — needs it now.
        const std::optional<Timestamp> due = buffer_.next_due();
        sooner = sleeping_until_ && due && *due < *sleeping_until_;
    }
    if (sooner) wake_.notify_one();
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
    std::vector<EncodedFrame> due;
    std::vector<uint8_t> joined;
    std::unique_lock lock(mutex_);
    while (running_) {
        while (std::optional<EncodedFrame> frame = buffer_.pop(SteadyClock::instance().now())) {
            due.push_back(std::move(*frame));
        }
        if (!due.empty()) {
            lock.unlock();
            write(due, joined);
            due.clear();
            lock.lock();
            continue;
        }
        const std::optional<Timestamp> next = buffer_.next_due();
        if (!next && finishing_) return;  // all written
        sleeping_until_ = next.value_or(Timestamp::max());
        if (next) {
            wake_.wait_until(lock, *next);
        } else {
            wake_.wait(lock);
        }
        sleeping_until_.reset();
    }
}

// The frames due together go out in one write: datagrams of seven packets, not
// one or more for each frame.
void DataPlayer::write(std::span<const EncodedFrame> frames, std::vector<uint8_t>& joined) {
    std::span<const uint8_t> bytes = frames.front().payload.bytes();
    if (frames.size() > 1) {
        joined.clear();
        for (const EncodedFrame& frame : frames) {
            joined.insert(joined.end(), frame.payload.bytes().begin(), frame.payload.bytes().end());
        }
        bytes = joined;
    }
    const Result<void> written = output_.write(bytes);
    std::lock_guard lock(mutex_);
    if (written) {
        bytes_ += bytes.size();
    } else if (write_errors_++ == 0) {
        LOG_WARN("client", "stream " << description_.id
                                     << ": cannot write out: " << to_string(written.error()));
    }
}

}  // namespace ratcast
