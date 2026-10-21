#include "ratcast/client/data_publisher.h"

#include <algorithm>
#include <utility>

#include "librats/util/logger.h"

namespace ratcast {

DataPublisher::DataPublisher(MediaSession& session, DataPublisherConfig config)
    : session_(session), config_(std::move(config)) {}

DataPublisher::~DataPublisher() { stop(); }

Result<void> DataPublisher::start(TsInput& input, EndHandler ended) {
    std::lock_guard lifecycle(lifecycle_);
    if (input_ || config_.packets_per_frame == 0) return fail(Errc::InvalidArgument);

    StreamDescription description;
    description.id = config_.stream;
    description.codec = Codec::MpegTs;
    description.format = DataFormat{};
    description.lifetime = config_.lifetime;
    description.label = config_.label;
    if (Result<void> added = session_.add_stream(description); !added) return added;

    finder_ = {};
    held_ = {};
    next_number_ = 0;
    Result<void> started = input.start(
        [this](std::span<const uint8_t> bytes, Timestamp received) { take(bytes, received); },
        [this, ended = std::move(ended)](Result<void> outcome) {
            send_held();  // the input's last
            if (ended) ended(std::move(outcome));
        },
        config_.hold);
    if (!started) {
        session_.remove_stream(config_.stream);
        return started;
    }
    input_ = &input;
    return {};
}

void DataPublisher::stop() {
    std::lock_guard lifecycle(lifecycle_);
    if (!input_) return;
    input_->stop();  // nothing is read after this
    input_ = nullptr;
    send_held();
    session_.remove_stream(config_.stream);
}

DataPublisherStats DataPublisher::stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

// The input's thread. Whole frames go at once; the packets that do not fill
// one wait for the rest, and go as they are once the input goes quiet.
void DataPublisher::take(std::span<const uint8_t> bytes, Timestamp received) {
    if (bytes.empty()) {
        send_held();
        return;
    }
    const size_t frame_size = config_.packets_per_frame * kTsPacketSize;
    uint64_t packets = 0;
    const uint64_t skipped = finder_.take(bytes, [&](std::span<const uint8_t> run) {
        packets += run.size() / kTsPacketSize;
        while (!run.empty()) {
            if (held_.empty()) {
                held_ = allocate_frame_payload(frame_size);
                held_since_ = received;
            }
            const size_t size = std::min(run.size(), frame_size - held_.size());
            held_.append(run.first(size));
            run = run.subspan(size);
            if (held_.size() == frame_size) send_held();
        }
    });
    std::lock_guard lock(stats_mutex_);
    stats_.bytes_read += bytes.size();
    stats_.packets += packets;
    stats_.skipped += skipped;
}

// The frame held, if any: its capture time when its first packet was read.
void DataPublisher::send_held() {
    if (held_.empty()) return;
    FrameInfo info;
    info.number = next_number_++;
    info.capture_time = held_since_;
    info.keyframe = true;  // every frame stands alone
    const Result<void> sent = session_.send_frame(config_.stream, info, std::exchange(held_, {}));
    std::lock_guard lock(stats_mutex_);
    if (sent) {
        ++stats_.frames;
    } else if (stats_.errors++ == 0) {
        LOG_WARN("client", "stream " << config_.stream << ": frame " << info.number
                                     << " not sent: " << to_string(sent.error()));
    }
}

}  // namespace ratcast
