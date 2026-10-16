#include <algorithm>
#include <cassert>
#include <chrono>

#include "ratcast/engine/pattern_source.h"

namespace ratcast {

PatternSource::PatternSource(PatternConfig config, Clock& clock) : config_(config), clock_(clock) {
    // As every source: 4:2:0 pictures of even sizes.
    assert(config.format == PixelFormat::BGRA || (config.width % 2 == 0 && config.height % 2 == 0));
}

PatternSource::~PatternSource() { stop(); }

Result<void> PatternSource::start(Sink sink, EndHandler /*ended*/) {
    std::lock_guard lock(mutex_);
    if (thread_.joinable()) return fail(Errc::InvalidArgument);
    running_ = true;
    thread_ = std::thread(&PatternSource::run, this, std::move(sink));
    return {};
}

void PatternSource::stop() {
    std::lock_guard lock(mutex_);
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void PatternSource::run(Sink sink) {
    using std::chrono::steady_clock;
    const auto interval = std::chrono::duration_cast<steady_clock::duration>(
        std::chrono::duration<double>(double(config_.frame_rate_den) / config_.frame_rate_num));

    // Due times count from the start rather than from the last picture, so a
    // slow sink makes the source skip pictures instead of drifting.
    const steady_clock::time_point start = steady_clock::now();
    uint64_t index = 0;
    while (running_.load(std::memory_order_relaxed)) {
        sink(draw_pattern(config_, index, clock_.now()));

        const auto elapsed = steady_clock::now() - start;
        index = static_cast<uint64_t>(elapsed / interval) + 1;
        const steady_clock::time_point due = start + interval * static_cast<int64_t>(index);
        // Short sleeps, so that stop() is never kept waiting long.
        while (running_.load(std::memory_order_relaxed) && steady_clock::now() < due) {
            std::this_thread::sleep_until(
                std::min(due, steady_clock::now() + std::chrono::milliseconds(5)));
        }
    }
}

}  // namespace ratcast
