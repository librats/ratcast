#include "ratcast/engine/virtual_output.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace ratcast {

VirtualOutput::VirtualOutput(VirtualOutputConfig config, Tap tap, Clock& clock)
    : config_(config), tap_(std::move(tap)), clock_(clock) {}

VirtualOutput::~VirtualOutput() { stop(); }

Result<void> VirtualOutput::start(Pull pull, EndHandler /*ended*/) {
    std::lock_guard lock(mutex_);
    if (config_.format.channels == 0 || config_.format.sample_rate == 0 || !pull) {
        return fail(Errc::InvalidArgument);
    }
    if (thread_.joinable()) return fail(Errc::InvalidArgument);
    running_ = true;
    thread_ = std::thread(&VirtualOutput::run, this, std::move(pull));
    return {};
}

void VirtualOutput::stop() {
    std::lock_guard lock(mutex_);
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void VirtualOutput::run(Pull pull) {
    using std::chrono::steady_clock;
    const uint32_t rate = config_.format.sample_rate;
    const size_t block = std::max<size_t>(1, audio_length(config_.block, rate));
    std::vector<float> samples(block * config_.format.channels);

    // Each block is pulled as the one before it runs out, counted from the
    // start: like a device, the output neither drifts nor skips.
    const steady_clock::time_point start = steady_clock::now();
    for (uint64_t sample = 0; running_.load(std::memory_order_relaxed); sample += block) {
        const steady_clock::time_point due =
            start +
            std::chrono::duration_cast<steady_clock::duration>(audio_duration(sample, rate));
        // Short sleeps, so that stop() is never kept waiting long.
        while (running_.load(std::memory_order_relaxed) && steady_clock::now() < due) {
            std::this_thread::sleep_until(
                std::min(due, steady_clock::now() + std::chrono::milliseconds(5)));
        }
        if (!running_.load(std::memory_order_relaxed)) break;
        const Timestamp play_time = clock_.now() + config_.latency;
        pull(samples, play_time);
        if (tap_) tap_(samples, play_time);
    }
}

}  // namespace ratcast
