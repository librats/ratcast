#include "ratcast/client/data_publisher.h"

#include <cstring>
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
    next_number_ = 0;
    Result<void> started = input.start(
        [this](std::span<const uint8_t> bytes, Timestamp received) { take(bytes, received); },
        std::move(ended));
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
    session_.remove_stream(config_.stream);
}

DataPublisherStats DataPublisher::stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

// The input's thread: the whole packets read so far go out now.
void DataPublisher::take(std::span<const uint8_t> bytes, Timestamp received) {
    uint64_t packets = 0;
    const uint64_t skipped = finder_.take(bytes, [&](std::span<const uint8_t> run) {
        packets += run.size() / kTsPacketSize;
        send(run, received);
    });
    std::lock_guard lock(stats_mutex_);
    stats_.bytes_read += bytes.size();
    stats_.packets += packets;
    stats_.skipped += skipped;
}

// `packets`, whole ones, in as few frames as fit them, as even as they come.
void DataPublisher::send(std::span<const uint8_t> packets, Timestamp received) {
    const size_t count = packets.size() / kTsPacketSize;
    if (count == 0) return;
    const size_t frames = (count + config_.packets_per_frame - 1) / config_.packets_per_frame;
    uint64_t sent = 0;
    uint64_t errors = 0;
    for (size_t i = 0; i < frames; ++i) {
        const size_t size = (count / frames + (i < count % frames ? 1 : 0)) * kTsPacketSize;
        MutableBuffer payload = allocate_frame_payload(size);
        std::memcpy(payload.append(size), packets.data(), size);
        packets = packets.subspan(size);

        FrameInfo info;
        info.number = next_number_++;
        info.capture_time = received;
        info.keyframe = true;  // every frame stands alone
        if (Result<void> result = session_.send_frame(config_.stream, info, std::move(payload));
            !result) {
            if (errors++ == 0) {
                LOG_WARN("client", "stream " << config_.stream << ": frame " << info.number
                                             << " not sent: " << to_string(result.error()));
            }
            continue;
        }
        ++sent;
    }
    std::lock_guard lock(stats_mutex_);
    stats_.frames += sent;
    stats_.errors += errors;
}

}  // namespace ratcast
