#pragma once

/**
 * @file delivery_thread.h
 * @brief A DeliveryBuffer played out on a thread of its own.
 *
 * Frames go in on the network's threads as they arrive; the thread hands each
 * on to the sink when it is due — in order, evenly, a fixed latency past the
 * fastest — and skips what did not come in time (DeliveryBuffer).
 *
 * The thread sleeps until the next frame is due and is woken early only by a
 * frame due sooner than that — the first, or a repair that lets those behind
 * it go; frames due together (a burst read at once, or the frames a repair
 * held back) go to the sink in one call.
 */

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <thread>

#include "ratcast/core/time.h"
#include "ratcast/export.h"
#include "ratcast/stream/delivery_buffer.h"
#include "ratcast/stream/frame.h"

namespace ratcast {

class RATCAST_EXPORT DeliveryThread {
public:
    /// Frames due together, in order, valid only during the call. Runs on the
    /// thread, with no lock held.
    using Sink = std::function<void(std::span<const EncodedFrame> frames)>;
    /// Once finish()ed, when all that was held has gone to the sink — or was
    /// dropped by stop(): on the thread, which ends then.
    using Drained = std::function<void()>;

    /// A handler may call stop(), but must not destroy the DeliveryThread.
    DeliveryThread(DeliveryBuffer::Config config, Sink sink, Drained drained = {});
    ~DeliveryThread();
    DeliveryThread(const DeliveryThread&) = delete;
    DeliveryThread& operator=(const DeliveryThread&) = delete;

    /// Start afresh: an empty buffer, a new thread. Does nothing while the
    /// thread runs.
    void start();

    /// Stop, dropping what is held. When this returns the sink is not running
    /// and will not be called again — called from a handler, on the thread
    /// itself, it is not called again once that handler returns, and the
    /// thread ends then, joined by the next stop() or the destructor.
    void stop();

    /// A frame, received now. Ignored unless running and not finishing.
    void push(EncodedFrame frame);

    /// Nothing more comes: hand on what is held, each at its time, then tell
    /// `drained` and end. stop() does not wait for that: it drops what is
    /// held, and `drained` is told before it returns.
    void finish();

    DeliveryBuffer::Stats stats() const;

    /// Whether the thread has ended: drained once finish()ed, or stopped.
    bool ended() const noexcept { return ended_.load(std::memory_order_acquire); }

private:
    void run();
    bool on_thread() const noexcept;

    const Sink sink_;
    const Drained drained_;

    std::mutex lifecycle_;  ///< serialises start() and stop()
    std::thread thread_;

    mutable std::mutex mutex_;  ///< the buffer, between network and thread
    std::condition_variable wake_;
    DeliveryBuffer buffer_;
    bool running_ = false;
    bool finishing_ = false;
    /// When the thread wakes by itself, while it sleeps; max: only when woken.
    std::optional<Timestamp> sleeping_until_;
    std::atomic<bool> ended_{false};
    std::atomic<std::thread::id> runner_{};  ///< the thread's id, while it runs
};

}  // namespace ratcast
