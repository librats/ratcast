// Cameras on Linux: PipeWire's video sources of a camera's role, read on the
// user's own daemon (a sandbox would go through the camera portal instead).

#include "capture/camera_backend.h"

#include <pipewire/pipewire.h>

#include <cstring>
#include <string_view>

#include "librats/util/logger.h"
#include "linux/pipewire_capture.h"
#include "linux/pipewire_stream.h"

namespace ratcast {
namespace {

/// How long the daemon is given to list what it has.
constexpr int kListTimeoutSeconds = 2;

/// One look at the daemon's registry, on the calling thread: the cameras in
/// it once the daemon says it has listed everything.
class CameraLister {
public:
    ~CameraLister() {
        if (timer_) pw_loop_destroy_source(pw_main_loop_get_loop(loop_), timer_);
        if (registry_) {
            spa_hook_remove(&registry_listener_);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
        }
        if (core_) {
            spa_hook_remove(&core_listener_);
            pw_core_disconnect(core_);
        }
        if (context_) pw_context_destroy(context_);
        if (loop_) pw_main_loop_destroy(loop_);
    }

    Result<std::vector<CameraInfo>> list() {
        pipewire::init_pipewire();
        loop_ = pw_main_loop_new(nullptr);
        if (!loop_) return fail(Errc::Internal);
        context_ = pw_context_new(pw_main_loop_get_loop(loop_), nullptr, 0);
        if (!context_) return fail(Errc::Internal);
        core_ = pw_context_connect(context_, nullptr, 0);
        if (!core_) return fail(Errc::Unavailable);

        static constexpr pw_core_events kCoreEvents = [] {
            pw_core_events events{};
            events.version = PW_VERSION_CORE_EVENTS;
            events.done = [](void* self, uint32_t id, int seq) {
                auto* lister = static_cast<CameraLister*>(self);
                if (id == PW_ID_CORE && seq == lister->sync_) lister->finish(true);
            };
            events.error = [](void* self, uint32_t id, int, int res, const char*) {
                if (id == PW_ID_CORE && res == -EPIPE)
                    static_cast<CameraLister*>(self)->finish(false);
            };
            return events;
        }();
        pw_core_add_listener(core_, &core_listener_, &kCoreEvents, this);
        registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
        if (!registry_) return fail(Errc::Internal);
        static constexpr pw_registry_events kRegistryEvents = [] {
            pw_registry_events events{};
            events.version = PW_VERSION_REGISTRY_EVENTS;
            events.global = [](void* self, uint32_t id, uint32_t, const char* type, uint32_t,
                               const spa_dict* props) {
                static_cast<CameraLister*>(self)->on_global(id, type, props);
            };
            return events;
        }();
        pw_registry_add_listener(registry_, &registry_listener_, &kRegistryEvents, this);
        sync_ = pw_core_sync(core_, PW_ID_CORE, 0);

        timer_ = pw_loop_add_timer(
            pw_main_loop_get_loop(loop_),
            [](void* self, uint64_t) { static_cast<CameraLister*>(self)->finish(false); }, this);
        timespec timeout{kListTimeoutSeconds, 0};
        pw_loop_update_timer(pw_main_loop_get_loop(loop_), timer_, &timeout, nullptr, false);

        pw_main_loop_run(loop_);
        if (!listed_) return fail(Errc::Unavailable);
        return std::move(cameras_);
    }

private:
    void on_global(uint32_t id, const char* type, const spa_dict* props) {
        if (!type || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) return;
        const auto is = [&](const char* key, std::string_view value) {
            const char* found = spa_dict_lookup(props, key);
            return found && value == found;
        };
        if (!is(PW_KEY_MEDIA_CLASS, "Video/Source") || !is(PW_KEY_MEDIA_ROLE, "Camera")) return;
        const char* name = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        if (!name) name = spa_dict_lookup(props, PW_KEY_NODE_NICK);
        if (!name) name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        cameras_.push_back({.node = id, .name = name ? name : "Camera"});
    }

    void finish(bool listed) {
        listed_ = listed;
        pw_main_loop_quit(loop_);
    }

    pw_main_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    spa_source* timer_ = nullptr;
    spa_hook core_listener_{};
    spa_hook registry_listener_{};
    int sync_ = 0;
    bool listed_ = false;
    std::vector<CameraInfo> cameras_;
};

}  // namespace

namespace backend {

Result<std::vector<CameraInfo>> list_cameras() { return CameraLister().list(); }

Result<std::unique_ptr<VideoSource>> open_camera(const CameraOptions& options) {
    uint32_t node = options.node;
    if (node == 0) {
        Result<std::vector<CameraInfo>> cameras = list_cameras();
        if (!cameras) return fail(cameras.error());
        if (cameras->empty()) {
            LOG_INFO("engine", "no camera here");
            return fail(Errc::Unavailable);
        }
        node = cameras->front().node;
    }
    pipewire::CaptureConfig config;
    config.kind = pipewire::CaptureConfig::Kind::Camera;
    config.node = node;
    config.format = options.format;
    config.max_frame_rate = options.max_frame_rate;
    config.max_width = options.max_width;
    config.max_height = options.max_height;
    return std::unique_ptr<VideoSource>(std::make_unique<pipewire::Capture>(config));
}

}  // namespace backend
}  // namespace ratcast
