#include "ratcast/client/video_publisher.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include "librats/util/logger.h"

namespace ratcast {
namespace {

/// A source that has sent nothing for this long is taken to be showing a still
/// picture (a compositor sends only what changes).
constexpr auto kStillAfter = std::chrono::milliseconds(100);

/// How often the viewers are looked at between keyframes, for what they decode,
/// and for whether there are any (SizeAdapter::update()).
constexpr auto kViewersEvery = std::chrono::seconds(1);

/// How long pictures in GPU memory may still come larger than a new limit
/// before they are made smaller here: the source that draws them smaller
/// itself (a screen's) does so within a few pictures, and one made smaller
/// on the CPU in between would start the encoder over for memory, and again
/// for the GPU's. Those that come meanwhile are dropped.
constexpr auto kSourceFollows = std::chrono::milliseconds(500);

/// Frames a second the encoder is set to.
uint32_t frame_rate_of(const VideoEncoderConfig& config) {
    return config.frame_rate_num / std::max(config.frame_rate_den, 1u);
}

/// The GPU whose memory `picture` is in, if any.
std::shared_ptr<GpuDevice> device_of(const VideoFrame& picture) {
    return picture.gpu() ? picture.gpu()->device() : nullptr;
}

}  // namespace

Codec codec_for_viewers(Codec preferred, std::span<const std::vector<Codec>> viewers,
                        std::span<const Codec> unusable) {
    const auto all_decode = [&](Codec codec) {
        return std::ranges::find(unusable, codec) == unusable.end() &&
               std::ranges::all_of(viewers, [&](const std::vector<Codec>& decodes) {
                   return std::ranges::find(decodes, codec) != decodes.end();
               });
    };
    const Codec codecs[] = {preferred, Codec::AV1, Codec::H265, Codec::H264};
    for (const Codec codec : codecs) {
        if (all_decode(codec)) return codec;
    }
    for (const Codec codec : codecs) {
        if (std::ranges::find(unusable, codec) == unusable.end()) return codec;
    }
    return preferred;
}

VideoPublisher::VideoPublisher(MediaSession& session, VideoPublisherConfig config, Clock& clock)
    : session_(session),
      config_(std::move(config)),
      clock_(clock),
      asked_lines_(config_.max_lines),
      asked_adapt_(config_.adapt_size),
      codec_(config_.encoder.codec) {}

VideoPublisher::~VideoPublisher() { stop(); }

Result<void> VideoPublisher::start(VideoSource& source, EndHandler ended) {
    std::lock_guard lifecycle(lifecycle_);
    if (source_) return fail(Errc::InvalidArgument);
    uint32_t lines = 0;
    {
        std::lock_guard lock(mutex_);
        stopping_ = false;
        finished_ = false;
        keyframe_requested_ = false;
        have_picture_ = false;
        size_asked_ = true;  // the adapter starts over, at the most
        lines = asked_lines_;
    }
    lines_ = lines;
    source.limit_size(lines);
    // The stream is announced afresh, so rate control starts over.
    const std::optional<BitrateLimits> limits = bitrate_limits();
    bitrate_ = limits ? limits->start : config_.encoder.bitrate;
    ended_ = std::move(ended);
    thread_ = std::thread(&VideoPublisher::run, this);

    Result<void> started = start_source(source, ++sources_);
    if (!started) {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        thread_.join();
        return started;
    }
    std::lock_guard source_lock(source_mutex_);
    source_ = &source;
    return {};
}

Result<void> VideoPublisher::switch_source(VideoSource& next) {
    std::lock_guard lifecycle(lifecycle_);
    if (!source_) return fail(Errc::InvalidArgument);
    {
        std::lock_guard lock(mutex_);
        if (finished_) return fail(Errc::InvalidArgument);  // over, to be stopped
    }
    const uint64_t before = sources_;
    next.limit_size(lines_.load());
    if (Result<void> started = start_source(next, ++sources_); !started) {
        std::lock_guard lock(mutex_);
        current_ = before;
        return started;
    }
    source_->stop();  // what it showed last may still be encoded: it went before
    std::lock_guard source_lock(source_mutex_);
    source_ = &next;
    return {};
}

Result<void> VideoPublisher::start_source(VideoSource& source, uint64_t number) {
    // Its pictures from the first on: a screen shows one, then only what
    // changes. The one before is dropped meanwhile, as it is about to go.
    {
        std::lock_guard lock(mutex_);
        current_ = number;
    }
    return source.start([this, number](VideoFrame picture) { take(number, std::move(picture)); },
                        [this, number](Result<void> outcome) { finish(outcome, number); });
}

void VideoPublisher::stop() {
    std::lock_guard lifecycle(lifecycle_);
    if (!source_) return;
    source_->stop();  // no picture arrives after this
    {
        std::lock_guard source_lock(source_mutex_);
        source_ = nullptr;
    }
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
    adapter_.reset();
    last_ = VideoFrame{};
}

void VideoPublisher::request_keyframe() {
    {
        std::lock_guard lock(mutex_);
        keyframe_requested_ = true;
    }
    wake_.notify_one();
}

void VideoPublisher::set_bitrate(uint32_t bits_per_second) {
    if (bits_per_second > 0) bitrate_.store(bits_per_second, std::memory_order_relaxed);
}

void VideoPublisher::set_size(uint32_t max_lines, bool adapt) {
    {
        std::lock_guard lock(mutex_);
        if (max_lines == asked_lines_ && adapt == asked_adapt_) return;
        asked_lines_ = max_lines;
        asked_adapt_ = adapt;
        size_asked_ = true;
        // A still screen sends nothing: its last picture again, of the new
        // size, which starts a new encoder and so is a keyframe anyway.
        keyframe_requested_ = true;
    }
    wake_.notify_one();
}

uint32_t VideoPublisher::encoder_bitrate(uint32_t width, uint32_t height) const {
    const uint32_t bitrate = bitrate_.load(std::memory_order_relaxed);
    if (!config_.bitrate_by_size) return bitrate;
    return std::min(bitrate, bitrate_for_size(width, height, frame_rate_of(config_.encoder),
                                              config_.encoder.bitrate));
}

std::optional<BitrateLimits> VideoPublisher::bitrate_limits() const {
    if (!config_.rate_control) return std::nullopt;
    const uint32_t max =
        config_.bitrate_by_size
            ? bitrate_for_size(3840, 2160, frame_rate_of(config_.encoder), config_.encoder.bitrate)
            : config_.encoder.bitrate;
    const uint32_t start = std::min(config_.start_bitrate, max);
    return BitrateLimits{.min = std::min(config_.min_bitrate, start), .start = start, .max = max};
}

VideoPublisherStats VideoPublisher::stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

void VideoPublisher::take(uint64_t source, VideoFrame picture) {
    bool replaced = false;
    {
        std::lock_guard lock(mutex_);
        if (source != current_) return;  // switched from
        replaced = pending_.has_value();
        pending_ = std::move(picture);
    }
    wake_.notify_one();
    std::lock_guard lock(stats_mutex_);
    ++stats_.pictures;
    if (replaced) ++stats_.skipped;
}

void VideoPublisher::finish(Result<void> outcome, uint64_t source) {
    {
        std::lock_guard lock(mutex_);
        if (finished_ || (source != 0 && source != current_)) return;
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
        if (repeat) {
            // Later than the original by the time since, on the source's own
            // clock, whichever that is: capture times only ever move on.
            picture.set_capture_time(picture.capture_time() + (clock_.now() - last_encoded_at_));
        }

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
    follow_viewers(keyframe);
    // What the adapter judges the encoder by: all this thread spends on a
    // picture but opening the encoder (counted from its start over). Scaling
    // on the CPU in fit() makes the pictures come late as encoding does.
    const auto fitting = std::chrono::steady_clock::now();
    picture = fit(std::move(picture));
    const auto fitted =
        std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - fitting);
    if (picture.empty()) {  // dropped while its source makes them smaller
        if (keyframe) {
            std::lock_guard lock(mutex_);
            keyframe_requested_ = true;
        }
        return {};
    }
    if (!encoder_ || picture.width() != encoder_config_.width ||
        picture.height() != encoder_config_.height ||
        picture.color_space() != encoder_config_.color_space ||
        device_of(picture) != encoder_config_.device || codec_ != encoder_config_.codec) {
        Result<void> opened = open_encoder(picture);
        const bool following = config_.follow_viewers && config_.encoder.encoder.empty();
        while (!opened && following &&
               (opened.error() == Errc::Unavailable || opened.error() == Errc::Unsupported)) {
            // No encoder of this codec here: never it again, the next the
            // viewers decode instead, or any where they decode none alike.
            unusable_.push_back(codec_);
            const Codec next = codec_for_viewers(config_.encoder.codec, decodes_, unusable_);
            if (std::ranges::find(unusable_, next) != unusable_.end()) break;
            LOG_WARN("client", "stream " << config_.stream << ": no encoder for "
                                         << to_string(codec_) << ", " << to_string(next)
                                         << " instead");
            codec_ = next;
            opened = open_encoder(picture);
        }
        if (!opened) return opened;
    }
    if (const uint32_t bitrate = encoder_bitrate(picture.width(), picture.height());
        bitrate != encoder_config_.bitrate) {
        encoder_->set_bitrate(bitrate);
        encoder_config_.bitrate = bitrate;
        std::lock_guard lock(stats_mutex_);
        stats_.bitrate = bitrate;
    }

    const auto start = std::chrono::steady_clock::now();
    const Result<void> encoded = encoder_->encode(
        picture, keyframe, [this](EncodedVideo&& frame) { send(std::move(frame)); });
    const auto took =
        std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - start);
    ++taken_;
    busy_ += fitted + took;
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
    last_encoded_at_ = clock_.now();
    std::lock_guard lock(mutex_);
    have_picture_ = true;
    return {};
}

VideoFrame VideoPublisher::fit(VideoFrame picture) {
    bool asked = false;
    uint32_t max_lines = 0;
    bool adapt = false;
    {
        std::lock_guard lock(mutex_);
        asked = std::exchange(size_asked_, false);
        max_lines = asked_lines_;
        adapt = asked_adapt_;
    }
    if (asked) {
        if (adapt) {
            adapter_.emplace(SizeAdapterConfig{.max_lines = max_lines,
                                               .frame_rate = frame_rate_of(config_.encoder),
                                               .at_1080p60 = config_.encoder.bitrate},
                             clock_.now());
        } else {
            adapter_.reset();
        }
        if (max_lines != lines_.load()) {
            limit(max_lines);
        } else {
            std::lock_guard lock(stats_mutex_);
            stats_.lines = max_lines;
        }
    }

    const uint32_t lines = lines_.load();
    if (lines != 0 && std::min(picture.width(), picture.height()) > lines) {
        if (picture.gpu() && clock_.now() - limited_at_ < kSourceFollows) return {};
        const PictureSize size = fit_lines(picture.width(), picture.height(), lines);
        Result<VideoFrame> scaled = scaler_.scale(picture, size.width, size.height);
        if (scaled) {
            picture = std::move(*scaled);
            std::lock_guard lock(stats_mutex_);
            ++stats_.scaled;
        } else if (!std::exchange(scaling_failed_, true)) {
            LOG_WARN("client", "stream " << config_.stream << ": pictures of " << picture.width()
                                         << "x" << picture.height() << " sent as they are, not "
                                         << size.width << "x" << size.height << ": "
                                         << to_string(scaled.error()));
        }
    }

    if (adapter_) {
        EncoderCounts counts{.taken = taken_, .busy = busy_};
        {
            std::lock_guard lock(stats_mutex_);
            counts.pictures = stats_.pictures;
            counts.skipped = stats_.skipped;
        }
        const Timestamp now = clock_.now();
        if (!watched_seen_ || now - *watched_seen_ >= kViewersEvery) {
            watched_seen_ = now;
            watched_ = !session_.viewers(config_.stream).empty();
        }
        const uint32_t bitrate = bitrate_.load(std::memory_order_relaxed);
        const uint32_t next =
            adapter_->update(now, picture.width(), picture.height(), bitrate, counts, watched_);
        if (next != lines) {
            LOG_INFO("client", "stream " << config_.stream << ": sent at "
                                         << (next != 0 ? std::to_string(next) + " lines"
                                                       : std::string("the size it comes"))
                                         << " from now on, at " << bitrate << " bit/s, "
                                         << counts.skipped << " of " << counts.pictures
                                         << " pictures skipped so far");
            limit(next);
        }
    }
    return picture;
}

void VideoPublisher::limit(uint32_t lines) {
    lines_ = lines;
    limited_at_ = clock_.now();
    {
        std::lock_guard lock(stats_mutex_);
        stats_.lines = lines;
    }
    std::lock_guard lock(source_mutex_);
    if (source_) source_->limit_size(lines);
}

/// The codec its viewers all decode as they come, looked for with a keyframe
/// (`now`: a viewer joining asks for one) or once kViewersEvery has passed.
void VideoPublisher::follow_viewers(bool now) {
    if (!config_.follow_viewers || !config_.encoder.encoder.empty()) return;
    const Timestamp at = clock_.now();
    if (!now && viewers_seen_ && at - *viewers_seen_ < kViewersEvery) return;
    viewers_seen_ = at;

    std::vector<std::vector<Codec>> decodes;
    for (const PeerId& viewer : session_.viewers(config_.stream)) {
        std::optional<Presence> presence = session_.presence(viewer);
        if (presence && !presence->decodes.empty()) decodes.push_back(std::move(presence->decodes));
    }
    decodes_ = std::move(decodes);
    if (decodes_.empty()) return;  // nobody says: as it is
    const Codec codec = codec_for_viewers(config_.encoder.codec, decodes_, unusable_);
    if (codec == codec_) return;
    LOG_INFO("client", "stream " << config_.stream << ": " << to_string(codec)
                                 << ", which its viewers all decode as it comes");
    codec_ = codec;
}

Result<void> VideoPublisher::open_encoder(const VideoFrame& picture) {
    if (encoder_) {
        // What the old encoder still holds belongs before the new one's frames.
        (void)encoder_->flush([this](EncodedVideo&& frame) { send(std::move(frame)); });
        encoder_.reset();
    }

    VideoEncoderConfig config = config_.encoder;
    config.codec = codec_;
    config.width = picture.width();
    config.height = picture.height();
    config.color_space = picture.color_space();
    config.device = device_of(picture);  // read where it is, if the encoder can
    config.bitrate = encoder_bitrate(config.width, config.height);
    Result<std::unique_ptr<VideoEncoder>> encoder = open_video_encoder(config);
    if (!encoder) {
        LOG_WARN("client", "stream " << config_.stream << ": no encoder for " << config.width << "x"
                                     << config.height << ": " << to_string(encoder.error()));
        return fail(encoder.error());
    }
    encoder_ = std::move(*encoder);
    encoder_config_ = config;
    number_offset_ = next_number_;
    // Pictures that came while it opened were skipped: not for being slow.
    if (adapter_) adapter_->encoder_started(clock_.now());

    StreamDescription description;
    description.id = config_.stream;
    description.codec = config.codec;
    description.format =
        VideoFormat{config.width, config.height, config.frame_rate_num, config.frame_rate_den};
    description.lifetime = config_.lifetime;
    description.label = config_.label;
    const Result<void> announced = announced_ ? session_.update_stream(description)
                                              : session_.add_stream(description, bitrate_limits());
    if (!announced) return announced;
    announced_ = std::move(description);

    std::lock_guard lock(stats_mutex_);
    ++stats_.encoders;
    stats_.width = config.width;
    stats_.height = config.height;
    stats_.bitrate = config.bitrate;
    stats_.encoder = encoder_->info().name;
    stats_.gpu_input = encoder_->info().gpu_input;
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
    stats_.bytes += size;
    if (info.keyframe) {
        ++stats_.keyframes;
        stats_.keyframe_bytes += size;
    }
}

}  // namespace ratcast
