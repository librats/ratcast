// AudioProcessor on WebRTC's AudioProcessing: AEC3, its noise suppressor and
// AGC2, fed 10 ms at a time, deinterleaved.

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "ratcast/engine/audio_processing.h"

#include <modules/audio_processing/include/audio_processing.h>

namespace ratcast {
namespace {

constexpr Duration kChunk = std::chrono::milliseconds(10);
/// The most a delay is believed: past it, a clock went astray.
constexpr Duration kMaxDelay = std::chrono::milliseconds(500);

bool supported(const AudioFormat& format) {
    return format.sample_rate >= 8'000 && format.sample_rate <= 48'000 &&
           format.sample_rate % 100 == 0 && (format.channels == 1 || format.channels == 2);
}

/// Interleaved samples gathered into 10 ms chunks, handed over deinterleaved:
/// APM's shape.
class Chunker {
public:
    explicit Chunker(const AudioFormat& format)
        : format_(format),
          frames_(audio_length(kChunk, format.sample_rate)),
          planes_(format.channels, std::vector<float>(frames_)),
          pointers_(format.channels) {
        for (size_t c = 0; c < planes_.size(); ++c) pointers_[c] = planes_[c].data();
        pending_.reserve(frames_ * format.channels);
    }

    /// `chunk(planes)` for each complete chunk of what was left from before
    /// and `samples`; the rest waits for the next.
    template <typename Chunk>
    void feed(std::span<const float> samples, Chunk chunk) {
        const size_t whole = frames_ * format_.channels;
        size_t used = 0;
        while (used < samples.size()) {
            const size_t take = std::min(whole - pending_.size(), samples.size() - used);
            pending_.insert(pending_.end(), samples.begin() + used, samples.begin() + used + take);
            used += take;
            if (pending_.size() < whole) break;
            for (size_t i = 0; i < frames_; ++i) {
                for (size_t c = 0; c < format_.channels; ++c) {
                    planes_[c][i] = pending_[i * format_.channels + c];
                }
            }
            pending_.clear();
            chunk(pointers_.data());
        }
    }

    /// The chunk's planes back into `out`, interleaved.
    void interleave(std::span<float> out) const {
        for (size_t i = 0; i < frames_; ++i) {
            for (size_t c = 0; c < format_.channels; ++c) {
                out[i * format_.channels + c] = planes_[c][i];
            }
        }
    }

    size_t frames() const noexcept { return frames_; }
    size_t pending_frames() const noexcept { return pending_.size() / format_.channels; }

private:
    const AudioFormat format_;
    const size_t frames_;
    std::vector<std::vector<float>> planes_;
    std::vector<float*> pointers_;
    std::vector<float> pending_;
};

class WebRtcProcessor final : public AudioProcessor {
public:
    WebRtcProcessor(const AudioProcessingConfig& config,
                    rtc::scoped_refptr<webrtc::AudioProcessing> apm, Clock& clock)
        : config_(config),
          apm_(std::move(apm)),
          clock_(clock),
          capture_stream_(int(config.capture.sample_rate), config.capture.channels),
          playback_stream_(int(config.playback.sample_rate), config.playback.channels),
          capture_(config.capture),
          playback_(config.playback) {}

    void played(std::span<const float> samples, Timestamp play_time) override {
        // When the first of the samples waiting from before is heard.
        Timestamp chunk_start =
            play_time - audio_duration(playback_.pending_frames(), config_.playback.sample_rate);
        playback_.feed(samples, [&](float* const* planes) {
            apm_->ProcessReverseStream(planes, playback_stream_, playback_stream_, planes);
            // How far ahead of being heard the far side is told of.
            lead_us_.store((chunk_start - clock_.now()).count(), std::memory_order_relaxed);
            chunk_start += kChunk;
        });
    }

    std::optional<AudioFrame> process(const AudioFrame& block) override {
        if (capture_.pending_frames() == 0) next_capture_ = block.capture_time();
        const size_t channels = config_.capture.channels;
        const size_t complete =
            (capture_.pending_frames() + block.length()) / capture_.frames() * capture_.frames();
        if (complete == 0) {
            capture_.feed(block.samples(), [](float* const*) {});
            return std::nullopt;
        }
        MutableAudioFrame out = MutableAudioFrame::allocate(config_.capture, complete);
        const Timestamp first = next_capture_;
        size_t written = 0;
        capture_.feed(block.samples(), [&](float* const* planes) {
            const Duration lag = clock_.now() - next_capture_;
            const Duration lead{lead_us_.load(std::memory_order_relaxed)};
            const Duration delay = std::clamp(lead + lag, Duration{}, kMaxDelay);
            apm_->set_stream_delay_ms(int(delay.count() / 1000));
            apm_->ProcessStream(planes, capture_stream_, capture_stream_, planes);
            capture_.interleave(out.samples().subspan(written * channels));
            written += capture_.frames();
            next_capture_ += kChunk;
            voice_.store(apm_->GetStatistics().voice_detected.value_or(false),
                         std::memory_order_relaxed);
        });
        // What is left waits, captured from here on.
        if (capture_.pending_frames() > 0) {
            next_capture_ = block.capture_time() + block.duration() -
                            audio_duration(capture_.pending_frames(), config_.capture.sample_rate);
        }
        return std::move(out).freeze(first);
    }

    bool voice() const noexcept override { return voice_.load(std::memory_order_relaxed); }

private:
    const AudioProcessingConfig config_;
    rtc::scoped_refptr<webrtc::AudioProcessing> apm_;
    Clock& clock_;
    const webrtc::StreamConfig capture_stream_;
    const webrtc::StreamConfig playback_stream_;

    Chunker capture_;           ///< the capture thread's
    Chunker playback_;          ///< the output's
    Timestamp next_capture_{};  ///< when the next chunk's first sample was captured
    std::atomic<int64_t> lead_us_{0};
    std::atomic<bool> voice_{false};
};

}  // namespace

Result<std::unique_ptr<AudioProcessor>> open_audio_processor(const AudioProcessingConfig& config,
                                                             Clock& clock) {
    if (!supported(config.capture) || !supported(config.playback)) {
        return fail(Errc::InvalidArgument);
    }
    rtc::scoped_refptr<webrtc::AudioProcessing> apm(webrtc::AudioProcessingBuilder().Create());
    if (!apm) return fail(Errc::Internal);

    webrtc::AudioProcessing::Config settings;
    settings.echo_canceller.enabled = config.echo_cancellation;
    settings.noise_suppression.enabled = config.noise_suppression;
    settings.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
    settings.gain_controller2.enabled = config.gain_control;
    settings.gain_controller2.adaptive_digital.enabled = config.gain_control;
    settings.high_pass_filter.enabled = true;
    settings.voice_detection.enabled = true;
    settings.pipeline.multi_channel_render = config.playback.channels > 1;
    apm->ApplyConfig(settings);
    const webrtc::StreamConfig capture(int(config.capture.sample_rate), config.capture.channels);
    const webrtc::StreamConfig playback(int(config.playback.sample_rate), config.playback.channels);
    if (apm->Initialize({{capture, capture, playback, playback}}) !=
        webrtc::AudioProcessing::kNoError) {
        return fail(Errc::Internal);
    }
    return std::unique_ptr<AudioProcessor>(
        std::make_unique<WebRtcProcessor>(config, std::move(apm), clock));
}

}  // namespace ratcast
