#include "ratcast/client/voice.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

#include "librats/util/logger.h"

namespace ratcast {
namespace {

/// How long speaking() and hearing() hold after the sound stops.
constexpr Duration kHold = std::chrono::milliseconds(300);
/// Louder than this is someone there: -45 dBFS, as RMS.
constexpr float kAudible = 0.0056f;

bool audible(std::span<const float> samples) {
    if (samples.empty()) return false;
    double sum = 0;
    for (float s : samples) sum += double(s) * s;
    return std::sqrt(sum / double(samples.size())) > kAudible;
}

}  // namespace

struct VoiceChannel::State {
    State(const VoiceConfig& config, Clock& clock) : config(config), clock(clock) {
        AudioProcessingConfig processing = config.processing;
        processing.capture = config.microphone;
        processing.playback = config.speaker;
        Result<std::unique_ptr<AudioProcessor>> opened = open_audio_processor(processing, clock);
        if (opened) {
            processor = std::move(*opened);
        } else {
            LOG_WARN("client", "no voice processing (" << to_string(opened.error())
                                                       << "): the microphone goes as it is");
        }
    }

    /// Whether `when` was within kHold of now.
    bool recent(const std::atomic<int64_t>& when) const noexcept {
        const int64_t at = when.load(std::memory_order_relaxed);
        return at != 0 && clock.now().time_since_epoch().count() - at < kHold.count();
    }
    void mark(std::atomic<int64_t>& when) const noexcept {
        when.store(clock.now().time_since_epoch().count(), std::memory_order_relaxed);
    }

    const VoiceConfig config;
    Clock& clock;
    std::unique_ptr<AudioProcessor> processor;  ///< null: none
    std::atomic<bool> muted{false};
    std::atomic<int64_t> spoke_us{0};  ///< when a voice was last on the microphone
    std::atomic<int64_t> heard_us{0};  ///< when the speaker last played something
};

class VoiceChannel::Microphone final : public AudioSource {
public:
    Microphone(std::unique_ptr<AudioSource> device, std::shared_ptr<State> state)
        : device_(std::move(device)),
          state_(std::move(state)),
          processor_(device_->format() == state_->config.microphone ? state_->processor.get()
                                                                    : nullptr) {}
    ~Microphone() override { stop(); }

    const AudioFormat& format() const noexcept override { return device_->format(); }

    Result<void> start(Sink sink, EndHandler ended) override {
        return device_->start(
            [this, sink = std::move(sink)](AudioFrame block) {
                if (processor_) {
                    std::optional<AudioFrame> cleaned = processor_->process(block);
                    if (!cleaned) return;  // less than 10 ms so far
                    block = std::move(*cleaned);
                }
                const bool muted = state_->muted.load(std::memory_order_relaxed);
                const bool voice = processor_ ? processor_->voice() : audible(block.samples());
                if (voice && !muted) state_->mark(state_->spoke_us);
                sink(muted ? silence(block) : std::move(block));
            },
            std::move(ended));
    }

    void stop() override { device_->stop(); }

private:
    static AudioFrame silence(const AudioFrame& like) {
        MutableAudioFrame frame = MutableAudioFrame::allocate(like.format(), like.length());
        std::ranges::fill(frame.samples(), 0.0f);
        return std::move(frame).freeze(like.capture_time());
    }

    std::unique_ptr<AudioSource> device_;
    std::shared_ptr<State> state_;
    AudioProcessor* const processor_;  ///< the state's, if the format is its
};

class VoiceChannel::Speaker final : public AudioOutput {
public:
    Speaker(std::unique_ptr<AudioOutput> device, std::shared_ptr<State> state)
        : device_(std::move(device)),
          state_(std::move(state)),
          processor_(device_->format() == state_->config.speaker ? state_->processor.get()
                                                                 : nullptr) {}
    ~Speaker() override { stop(); }

    const AudioFormat& format() const noexcept override { return device_->format(); }

    Result<void> start(Pull pull, EndHandler ended) override {
        return device_->start(
            [this, pull = std::move(pull)](std::span<float> out, Timestamp play_time) {
                pull(out, play_time);
                if (processor_) processor_->played(out, play_time);
                if (audible(out)) state_->mark(state_->heard_us);
            },
            std::move(ended));
    }

    void stop() override { device_->stop(); }

private:
    std::unique_ptr<AudioOutput> device_;
    std::shared_ptr<State> state_;
    AudioProcessor* const processor_;
};

VoiceChannel::VoiceChannel(VoiceConfig config, Clock& clock)
    : state_(std::make_shared<State>(config, clock)) {}

std::unique_ptr<AudioSource> VoiceChannel::microphone(std::unique_ptr<AudioSource> device) {
    return std::make_unique<Microphone>(std::move(device), state_);
}

std::unique_ptr<AudioOutput> VoiceChannel::speaker(std::unique_ptr<AudioOutput> device) {
    return std::make_unique<Speaker>(std::move(device), state_);
}

void VoiceChannel::set_muted(bool muted) noexcept {
    state_->muted.store(muted, std::memory_order_relaxed);
}
bool VoiceChannel::muted() const noexcept { return state_->muted.load(std::memory_order_relaxed); }
bool VoiceChannel::speaking() const noexcept {
    return !muted() && state_->recent(state_->spoke_us);
}
bool VoiceChannel::hearing() const noexcept { return state_->recent(state_->heard_us); }
bool VoiceChannel::processed() const noexcept { return state_->processor != nullptr; }

Result<AudioPublisher*> publish_voice(Call& call, std::unique_ptr<AudioSource> microphone,
                                      StreamId stream) {
    AudioPublisherConfig voice;
    voice.stream = stream;
    voice.label = std::string(kVoiceLabel);
    voice.encoder.content = AudioContent::Voice;
    voice.encoder.bitrate = 32'000;
    voice.encoder.dtx = true;
    voice.encoder.expected_loss = 10;
    return call.publish(std::move(microphone), voice);
}

}  // namespace ratcast
