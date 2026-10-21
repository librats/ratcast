#include "ratcast/stream/delivery_thread.h"

#include <utility>
#include <vector>

namespace ratcast {

DeliveryThread::DeliveryThread(DeliveryBuffer::Config config, Sink sink, Drained drained)
    : sink_(std::move(sink)), drained_(std::move(drained)), buffer_(config) {}

DeliveryThread::~DeliveryThread() { stop(); }

void DeliveryThread::start() {
    std::lock_guard lifecycle(lifecycle_);
    if (thread_.joinable()) {
        if (!ended() || on_thread()) return;
        thread_.join();
    }
    ended_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock(mutex_);
        buffer_.reset();
        running_ = true;
        finishing_ = false;
    }
    thread_ = std::thread(&DeliveryThread::run, this);
}

void DeliveryThread::stop() {
    if (on_thread()) {
        // From a handler: the thread ends once it returns, and is joined
        // later. Not under lifecycle_, which a stop() joining it may hold.
        std::lock_guard lock(mutex_);
        running_ = false;
        return;
    }
    std::lock_guard lifecycle(lifecycle_);
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    wake_.notify_one();
    thread_.join();
}

void DeliveryThread::push(EncodedFrame frame) {
    const Timestamp now = SteadyClock::instance().now();
    bool sooner = false;
    {
        std::lock_guard lock(mutex_);
        if (!running_ || finishing_) return;
        buffer_.push(std::move(frame), now);
        // Most frames fall due after the one the thread waits for, which wakes
        // it anyway; only one due sooner — the first, or a repair — needs it now.
        const std::optional<Timestamp> due = buffer_.next_due();
        sooner = sleeping_until_ && due && *due < *sleeping_until_;
    }
    if (sooner) wake_.notify_one();
}

void DeliveryThread::finish() {
    {
        std::lock_guard lock(mutex_);
        finishing_ = true;
    }
    wake_.notify_one();
}

DeliveryBuffer::Stats DeliveryThread::stats() const {
    std::lock_guard lock(mutex_);
    return buffer_.stats();
}

// Hand each frame on as it falls due; sleep until the next one is, or a frame
// comes that may be due sooner.
bool DeliveryThread::on_thread() const noexcept {
    return runner_.load(std::memory_order_acquire) == std::this_thread::get_id();
}

void DeliveryThread::run() {
    runner_.store(std::this_thread::get_id(), std::memory_order_release);
    std::vector<EncodedFrame> due;
    std::unique_lock lock(mutex_);
    while (running_) {
        while (std::optional<EncodedFrame> frame = buffer_.pop(SteadyClock::instance().now())) {
            due.push_back(std::move(*frame));
        }
        if (!due.empty()) {
            lock.unlock();
            sink_(due);
            due.clear();
            lock.lock();
            continue;
        }
        const std::optional<Timestamp> next = buffer_.next_due();
        if (!next && finishing_) break;  // all handed on
        sleeping_until_ = next.value_or(Timestamp::max());
        if (next) {
            wake_.wait_until(lock, *next);
        } else {
            wake_.wait(lock);
        }
        sleeping_until_.reset();
    }
    const bool finished = finishing_;
    lock.unlock();
    if (finished && drained_) drained_();
    runner_.store({}, std::memory_order_release);
    ended_.store(true, std::memory_order_release);
}

}  // namespace ratcast
