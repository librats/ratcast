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
      delivery_({.latency = latency_},
                [this](std::span<const EncodedFrame> frames) { write(frames); }) {}

DataPlayer::~DataPlayer() { stop(); }

Result<void> DataPlayer::start() {
    std::lock_guard lifecycle(lifecycle_);
    if (started_ || description_.kind() != MediaKind::Data) return fail(Errc::InvalidArgument);
    delivery_.start();
    if (Result<void> subscribed = session_.subscribe(peer_, description_.id); !subscribed) {
        delivery_.stop();
        return subscribed;
    }
    started_ = true;
    return {};
}

void DataPlayer::stop() {
    std::lock_guard lifecycle(lifecycle_);
    if (!started_) return;
    session_.unsubscribe(peer_, description_.id);
    delivery_.stop();
    started_ = false;
}

void DataPlayer::push(EncodedFrame frame) { delivery_.push(std::move(frame)); }

void DataPlayer::finish() { delivery_.finish(); }

DataPlayerStats DataPlayer::stats() const {
    std::lock_guard lock(stats_mutex_);
    return {delivery_.stats(), bytes_, write_errors_, latency_};
}

// The frames due together go out in one write: datagrams of seven packets, not
// one or more for each frame.
void DataPlayer::write(std::span<const EncodedFrame> frames) {
    std::span<const uint8_t> bytes = frames.front().payload.bytes();
    if (frames.size() > 1) {
        joined_.clear();
        for (const EncodedFrame& frame : frames) {
            joined_.insert(joined_.end(), frame.payload.bytes().begin(),
                           frame.payload.bytes().end());
        }
        bytes = joined_;
    }
    const Result<void> written = output_.write(bytes);
    std::lock_guard lock(stats_mutex_);
    if (written) {
        bytes_ += bytes.size();
    } else if (write_errors_++ == 0) {
        LOG_WARN("client", "stream " << description_.id
                                     << ": cannot write out: " << to_string(written.error()));
    }
}

}  // namespace ratcast
