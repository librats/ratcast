#include "ratcast/client/audio_player.h"

#include <algorithm>
#include <utility>

#include "librats/util/logger.h"

namespace ratcast {
namespace {

/// The shortest frame the engine's encoder makes: what the jitter buffer's
/// room is counted in.
constexpr Duration kShortestFrame = std::chrono::milliseconds(10);

JitterBuffer::Config jitter_config(const StreamDescription& description,
                                   const AudioPlayerConfig& config) {
    JitterBuffer::Config jitter;
    jitter.max_delay = config.max_delay.value_or(description.lifetime);
    jitter.min_delay = std::min(jitter.min_delay, jitter.max_delay);
    // Room for every frame a delay of max_delay holds, and a few more.
    jitter.max_frames =
        std::max(jitter.max_frames, static_cast<size_t>(jitter.max_delay / kShortestFrame) + 8);
    return jitter;
}

/// `duration` of silence: what is played where the decoder had nothing to give.
AudioFrame silence(const AudioFormat& format, Duration duration, Timestamp capture_time) {
    MutableAudioFrame frame = MutableAudioFrame::allocate(
        format, std::max<size_t>(1, audio_length(duration, format.sample_rate)));
    std::ranges::fill(frame.samples(), 0.0f);
    return std::move(frame).freeze(capture_time);
}

}  // namespace

AudioPlayer::AudioPlayer(MediaSession& session, PeerId peer, StreamDescription description,
                         AudioOutput& output, AudioPlayerConfig config, Observer observer,
                         Clock& clock)
    : session_(session),
      peer_(std::move(peer)),
      description_(std::move(description)),
      output_(output),
      observer_(std::move(observer)),
      clock_(clock),
      jitter_(jitter_config(description_, config)) {}

AudioPlayer::~AudioPlayer() { stop(); }

Result<void> AudioPlayer::start() {
    std::lock_guard lifecycle(lifecycle_);
    if (started_ || description_.kind() != MediaKind::Audio) return fail(Errc::InvalidArgument);
    Result<std::unique_ptr<AudioDecoder>> decoder =
        open_audio_decoder({description_.codec, output_.format(), description_.codec_config});
    if (!decoder) {
        LOG_WARN("client", "stream " << description_.id
                                     << ": no audio decoder: " << to_string(decoder.error()));
        return fail(decoder.error());
    }
    decoder_ = std::move(*decoder);
    pending_ = AudioFrame{};
    pending_played_ = 0;
    {
        std::lock_guard lock(mutex_);
        jitter_.reset();
        stats_.decoder = decoder_->info().name;
    }
    if (Result<void> started = output_.start(
            [this](std::span<float> out, Timestamp play_time) { pull(out, play_time); }, {});
        !started) {
        decoder_.reset();
        return started;
    }
    if (Result<void> subscribed = session_.subscribe(peer_, description_.id); !subscribed) {
        output_.stop();
        decoder_.reset();
        return subscribed;
    }
    started_ = true;
    return {};
}

void AudioPlayer::stop() {
    std::lock_guard lifecycle(lifecycle_);
    if (!started_) return;
    started_ = false;
    session_.unsubscribe(peer_, description_.id);
    output_.stop();
    decoder_.reset();
}

void AudioPlayer::push(EncodedFrame frame) {
    const Timestamp now = clock_.now();
    std::lock_guard lock(mutex_);
    ++stats_.frames;
    jitter_.push(std::move(frame), now);
}

AudioPlayerStats AudioPlayer::stats() const {
    std::lock_guard lock(mutex_);
    AudioPlayerStats stats = stats_;
    stats.jitter = jitter_.stats();
    stats.delay = jitter_.delay();
    stats.target = jitter_.target();
    return stats;
}

// The output's thread: fill `out` with what comes next, one frame or gap at a
// time, each asked for as the sound before it in `out` runs out.
void AudioPlayer::pull(std::span<float> out, Timestamp play_time) {
    const AudioFormat& format = output_.format();
    const size_t length = out.size() / format.channels;
    const Timestamp now = clock_.now();
    size_t filled = 0;  // samples per channel
    while (filled < length) {
        if (pending_played_ < pending_.length()) {
            const size_t n = std::min(length - filled, pending_.length() - pending_played_);
            std::ranges::copy(
                pending_.samples().subspan(pending_played_ * format.channels, n * format.channels),
                out.begin() + static_cast<ptrdiff_t>(filled * format.channels));
            pending_played_ += n;
            filled += n;
            continue;
        }
        const Duration offset = audio_duration(filled, format.sample_rate);
        JitterBuffer::Next next;
        {
            std::lock_guard lock(mutex_);
            next = jitter_.next(now + offset);
        }
        if (next.kind == JitterBuffer::Next::Kind::Wait) {
            std::fill(out.begin() + static_cast<ptrdiff_t>(filled * format.channels), out.end(),
                      0.0f);
            break;
        }
        pending_ = render(next);
        pending_played_ = 0;
        if (next.kind == JitterBuffer::Next::Kind::Frame && observer_) {
            observer_(next.capture_time, play_time + offset);
        }
    }
    const AudioDecoderStats decoded = decoder_->stats();
    std::lock_guard lock(mutex_);
    stats_.decoded = decoded;
}

// Sound for `next`: the frame decoded, or the gap made up — from FEC where the
// frame after it carries some. Never empty, so that playing goes on.
AudioFrame AudioPlayer::render(const JitterBuffer::Next& next) {
    using Kind = JitterBuffer::Next::Kind;
    if (next.kind == Kind::Frame) {
        if (Result<AudioFrame> decoded = decoder_->decode(*next.frame);
            decoded && !decoded->empty()) {
            return std::move(*decoded);
        }
        std::lock_guard lock(mutex_);
        ++stats_.decode_errors;
    }
    // A gap, or a frame that would not decode.
    const EncodedFrame* after = next.kind == Kind::Gap && next.frame ? &*next.frame : nullptr;
    if (Result<AudioFrame> made_up = decoder_->conceal(next.duration, next.capture_time, after);
        made_up && !made_up->empty()) {
        return std::move(*made_up);
    }
    return silence(output_.format(), next.duration, next.capture_time);
}

}  // namespace ratcast
