#include "linux/pipewire_stream.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>

#include "librats/util/logger.h"

namespace ratcast::pipewire {

void init_pipewire() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

StreamThread::StreamThread(Events& events, EndHandler ended)
    : events_(events), ended_(std::move(ended)) {}

StreamThread::~StreamThread() {
    if (!loop_) return;
    // PipeWire (1.6) leaks a stream destroyed while it is still being set up
    // on the daemon, which a stream that ends at once can be.
    if (stream_) {
        pw_thread_loop_lock(loop_);
        for (int second = 0;
             second < 2 && pw_stream_get_state(stream_, nullptr) == PW_STREAM_STATE_CONNECTING;
             ++second) {
            pw_thread_loop_timed_wait(loop_, 1);
        }
        pw_thread_loop_unlock(loop_);
    }
    // The loop thread next, then the stream, which stops its data thread:
    // past that nothing calls back, and the rest is torn down here, the way
    // PipeWire's own thread-loop examples do.
    pw_thread_loop_stop(loop_);
    if (stream_) {
        spa_hook_remove(&stream_listener_);
        pw_stream_destroy(stream_);
    }
    if (registry_) {
        spa_hook_remove(&registry_listener_);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
    }
    if (core_) {
        spa_hook_remove(&core_listener_);
        pw_core_disconnect(core_);
    }
    if (context_) pw_context_destroy(context_);
    pw_thread_loop_destroy(loop_);
}

Result<void> StreamThread::open(const Config& config, pw_properties* properties,
                                std::span<const spa_pod*> params) {
    init_pipewire();
    loop_ = pw_thread_loop_new(config.name, nullptr);
    if (!loop_) {
        pw_properties_free(properties);
        return fail(Errc::Internal);
    }
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_ || pw_thread_loop_start(loop_) < 0) {
        pw_properties_free(properties);
        return fail(Errc::Internal);
    }

    pw_thread_loop_lock(loop_);
    const Result<void> connected = connect(config, properties, params);
    pw_thread_loop_unlock(loop_);
    return connected;
}

uint32_t StreamThread::node_id() const {
    if (!stream_) return 0;
    pw_thread_loop_lock(loop_);
    const uint32_t id = pw_stream_get_node_id(stream_);
    pw_thread_loop_unlock(loop_);
    return id == SPA_ID_INVALID ? 0 : id;
}

Result<void> StreamThread::connect(const Config& config, pw_properties* properties,
                                   std::span<const spa_pod*> params) {
    if (config.fd >= 0) {
        const int fd = fcntl(config.fd, F_DUPFD_CLOEXEC, 3);
        core_ = fd >= 0 ? pw_context_connect_fd(context_, fd, nullptr, 0) : nullptr;
    } else {
        core_ = pw_context_connect(context_, nullptr, 0);
    }
    if (!core_) {
        LOG_WARN("engine", "cannot connect to PipeWire: " << strerror(errno));
        pw_properties_free(properties);
        return fail(Errc::Unavailable);
    }
    static constexpr pw_core_events kCoreEvents = [] {
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.done = [](void* self, uint32_t id, int seq) {
            if (id == PW_ID_CORE) static_cast<StreamThread*>(self)->on_core_done(seq);
        };
        events.error = [](void* self, uint32_t id, int, int res, const char* message) {
            if (id == PW_ID_CORE) static_cast<StreamThread*>(self)->on_core_error(res, message);
        };
        return events;
    }();
    pw_core_add_listener(core_, &core_listener_, &kCoreEvents, this);

    // The stream does not tell a node that went from one that is idle: the
    // registry does, and says too whether the node was there at all.
    node_ = config.node;
    if (node_ != 0) {
        registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
        if (!registry_) {
            pw_properties_free(properties);
            return fail(Errc::Internal);
        }
        static constexpr pw_registry_events kRegistryEvents = [] {
            pw_registry_events events{};
            events.version = PW_VERSION_REGISTRY_EVENTS;
            events.global = [](void* self, uint32_t id, uint32_t, const char* type, uint32_t,
                               const spa_dict*) {
                static_cast<StreamThread*>(self)->on_global(id, type);
            };
            events.global_remove = [](void* self, uint32_t id) {
                static_cast<StreamThread*>(self)->on_global_remove(id);
            };
            return events;
        }();
        pw_registry_add_listener(registry_, &registry_listener_, &kRegistryEvents, this);
        registry_sync_ = pw_core_sync(core_, PW_ID_CORE, 0);
    }

    stream_ = pw_stream_new(core_, config.name, properties);
    if (!stream_) return fail(Errc::Internal);
    static constexpr pw_stream_events kStreamEvents = [] {
        pw_stream_events events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = [](void* self, pw_stream_state old, pw_stream_state state,
                                  const char* error) {
            static_cast<StreamThread*>(self)->on_state_changed(old, state, error);
        };
        events.param_changed = [](void* self, uint32_t id, const spa_pod* param) {
            static_cast<StreamThread*>(self)->events_.on_param_changed(id, param);
        };
        events.process = [](void* self) { static_cast<StreamThread*>(self)->events_.on_process(); };
        return events;
    }();
    pw_stream_add_listener(stream_, &stream_listener_, &kStreamEvents, this);

    // A stream pointed at a node goes with it: it never moves on to another.
    auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                              PW_STREAM_FLAG_MAP_BUFFERS | config.flags);
    if (node_ != 0) flags = static_cast<pw_stream_flags>(flags | PW_STREAM_FLAG_DONT_RECONNECT);
    if (const int error =
            pw_stream_connect(stream_, config.direction, node_ != 0 ? node_ : PW_ID_ANY, flags,
                              params.data(), static_cast<uint32_t>(params.size()));
        error < 0) {
        LOG_WARN("engine", "cannot connect to PipeWire node " << node_ << ": " << strerror(-error));
        return fail(Errc::Unavailable);
    }
    return {};
}

void StreamThread::on_global(uint32_t id, const char* type) {
    if (id == node_ && type && std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) node_seen_ = true;
}

void StreamThread::on_global_remove(uint32_t id) {
    if (id == node_) finish({});  // the window closed, sharing stopped, the device went
}

/// The registry has listed what there was when the stream started.
void StreamThread::on_core_done(int seq) {
    if (registry_ == nullptr || seq != registry_sync_ || node_seen_) return;
    LOG_WARN("engine", "PipeWire node " << node_ << " does not exist");
    finish(fail(Errc::Unavailable));
}

void StreamThread::on_core_error(int res, const char* message) {
    LOG_WARN("engine", "PipeWire: " << (message ? message : "") << " (" << strerror(-res) << ")");
    finish(fail(Errc::Unavailable));
}

void StreamThread::on_state_changed(pw_stream_state old, pw_stream_state state, const char* error) {
    LOG_DEBUG("engine", "PipeWire stream: " << pw_stream_state_as_string(old) << " -> "
                                            << pw_stream_state_as_string(state));
    pw_thread_loop_signal(loop_, false);  // for a teardown waiting on it
    if (state == PW_STREAM_STATE_ERROR) {
        LOG_WARN("engine", "PipeWire stream failed: " << (error ? error : "unknown error"));
        finish(fail(Errc::Unavailable));
    }
}

void StreamThread::finish(Result<void> outcome) {
    if (finished_) return;
    finished_ = true;
    if (ended_) ended_(outcome);
}

}  // namespace ratcast::pipewire
