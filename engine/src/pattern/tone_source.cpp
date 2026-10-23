#include "ratcast/engine/tone_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

#include "sleeper.h"

namespace ratcast {

AudioFrame draw_tone(const ToneConfig& config, uint64_t first, size_t length,
                     Timestamp capture_time) {
    MutableAudioFrame frame = MutableAudioFrame::allocate(config.format, length);
    std::span<float> out = frame.samples();
    const size_t channels = config.format.channels;
    const double cycles_per_sample = double(config.frequency) / config.format.sample_rate;
    for (size_t i = 0; i < length; ++i) {
        // The phase from the sample's index alone, whole cycles dropped before
        // the sine so that it stays exact after hours of tone.
        const double phase = std::fmod(cycles_per_sample * double(first + i), 1.0);
        const auto v = static_cast<float>(config.level * std::sin(2 * std::numbers::pi * phase));
        std::fill_n(&out[i * channels], channels, v);
    }
    return std::move(frame).freeze(capture_time);
}

ToneSource::ToneSource(ToneConfig config, Clock& clock) : config_(config), clock_(clock) {}

ToneSource::~ToneSource() { stop(); }

Result<void> ToneSource::start(Sink sink, EndHandler /*ended*/) {
    std::lock_guard lock(mutex_);
    if (thread_.joinable()) return fail(Errc::InvalidArgument);
    running_ = true;
    thread_ = std::thread(&ToneSource::run, this, std::move(sink));
    return {};
}

void ToneSource::stop() {
    std::lock_guard lock(mutex_);
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void ToneSource::run(Sink sink) {
    using std::chrono::steady_clock;
    const uint32_t rate = config_.format.sample_rate;
    const size_t block = std::max<size_t>(1, audio_length(config_.block, rate));

    // Blocks are due as their last sample is, counted from the start: like a
    // device, the source neither drifts nor skips.
    const steady_clock::time_point start = steady_clock::now();
    const Timestamp first_capture = clock_.now();
    Sleeper sleeper;
    for (uint64_t sample = 0; running_.load(std::memory_order_relaxed); sample += block) {
        const steady_clock::time_point due =
            start + std::chrono::duration_cast<steady_clock::duration>(
                        audio_duration(sample + block, rate));
        // Short sleeps, so that stop() is never kept waiting long.
        while (running_.load(std::memory_order_relaxed) && steady_clock::now() < due) {
            sleeper.until(std::min(due, steady_clock::now() + std::chrono::milliseconds(5)));
        }
        if (!running_.load(std::memory_order_relaxed)) break;
        sink(draw_tone(config_, sample, block, first_capture + audio_duration(sample, rate)));
    }
}

}  // namespace ratcast
