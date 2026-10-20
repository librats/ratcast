#pragma once

/**
 * @file pipewire_stream.h
 * @brief One PipeWire stream on a thread loop of its own: what every capture
 *        and playback here has in common.
 *
 * Connecting — to the user's daemon, or over a connection a portal opened —
 * watching the node the stream is pointed at, so that the stream ends when the
 * node goes or was never there, reporting how it ended once, and tearing it
 * all down in the order PipeWire needs. What flows through the stream is the
 * owner's: it hears the stream's events through StreamThread::Events.
 *
 * PipeWire calls back on the loop's thread with the loop's lock held, or for a
 * stream with PW_STREAM_FLAG_RT_PROCESS, `process` on its realtime data
 * thread. After the StreamThread is destroyed nothing calls back; an owner
 * declares it last among its members so that it goes first.
 */

#include <pipewire/pipewire.h>

#include <cstdint>
#include <functional>
#include <span>

#include "ratcast/core/result.h"

namespace ratcast::pipewire {

/// pw_init(), once per process.
void init_pipewire();

class StreamThread {
public:
    class Events {
    public:
        /// A param of the stream was set; for SPA_PARAM_Format, the format
        /// agreed on (nullptr when it was cleared).
        virtual void on_param_changed(uint32_t /*id*/, const spa_pod* /*param*/) {}
        /// Buffers are ready to be dequeued.
        virtual void on_process() = 0;

    protected:
        ~Events() = default;
    };

    /// How the stream ended by itself: once, from a PipeWire thread, with
    /// success when the node went and Unavailable for any failure.
    using EndHandler = std::function<void(Result<void> outcome)>;

    struct Config {
        /// A connection to PipeWire (a portal's OpenPipeWireRemote), or -1 for
        /// the user's daemon. Duplicated, not taken over.
        int fd = -1;
        /// The node to connect to, which the stream ends with; 0 for whichever
        /// the session manager picks, which it follows as the default changes.
        uint32_t node = 0;
        const char* name = "ratcast";
        pw_direction direction = PW_DIRECTION_INPUT;
        /// Beyond AUTOCONNECT and MAP_BUFFERS, which every stream here has.
        pw_stream_flags flags = PW_STREAM_FLAG_NONE;
    };

    StreamThread(Events& events, EndHandler ended);
    ~StreamThread();
    StreamThread(const StreamThread&) = delete;
    StreamThread& operator=(const StreamThread&) = delete;

    /// Connect a stream with `properties` (taken over) offering `params`.
    /// Fails with Unavailable when PipeWire cannot be reached.
    Result<void> open(const Config& config, pw_properties* properties,
                      std::span<const spa_pod*> params);

    /// For the Events callbacks, which may use it as they like.
    pw_stream* stream() const noexcept { return stream_; }

    /// The stream's own node, once PipeWire has made it; 0 before.
    uint32_t node_id() const;

private:
    Result<void> connect(const Config& config, pw_properties* properties,
                         std::span<const spa_pod*> params);
    void on_global(uint32_t id, const char* type);
    void on_global_remove(uint32_t id);
    void on_core_done(int seq);
    void on_core_error(int res, const char* message);
    void on_state_changed(pw_stream_state old, pw_stream_state state, const char* error);
    void finish(Result<void> outcome);

    Events& events_;
    const EndHandler ended_;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook core_listener_{};
    spa_hook registry_listener_{};
    spa_hook stream_listener_{};

    // On the loop thread only.
    uint32_t node_ = 0;  ///< the node the stream is pointed at; 0 for any
    int registry_sync_ = 0;
    bool node_seen_ = false;
    bool finished_ = false;
};

}  // namespace ratcast::pipewire
