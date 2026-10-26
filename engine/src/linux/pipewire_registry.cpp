#include "linux/pipewire_registry.h"

#include <pipewire/pipewire.h>

#include <cerrno>
#include <cstring>

#include "linux/pipewire_stream.h"

namespace ratcast::pipewire {
namespace {

/// How long the daemon is given to list what it has.
constexpr int kListTimeoutSeconds = 2;

class NodeLister {
public:
    ~NodeLister() {
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

    Result<std::vector<NodeInfo>> list() {
        init_pipewire();
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
                auto* lister = static_cast<NodeLister*>(self);
                if (id == PW_ID_CORE && seq == lister->sync_) lister->finish(true);
            };
            events.error = [](void* self, uint32_t id, int, int res, const char*) {
                if (id == PW_ID_CORE && res == -EPIPE)
                    static_cast<NodeLister*>(self)->finish(false);
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
                static_cast<NodeLister*>(self)->on_global(id, type, props);
            };
            return events;
        }();
        pw_registry_add_listener(registry_, &registry_listener_, &kRegistryEvents, this);
        sync_ = pw_core_sync(core_, PW_ID_CORE, 0);

        timer_ = pw_loop_add_timer(
            pw_main_loop_get_loop(loop_),
            [](void* self, uint64_t) { static_cast<NodeLister*>(self)->finish(false); }, this);
        timespec timeout{kListTimeoutSeconds, 0};
        pw_loop_update_timer(pw_main_loop_get_loop(loop_), timer_, &timeout, nullptr, false);

        pw_main_loop_run(loop_);
        if (!listed_) return fail(Errc::Unavailable);
        return std::move(nodes_);
    }

private:
    void on_global(uint32_t id, const char* type, const spa_dict* props) {
        if (!type || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) return;
        const auto value = [&](const char* key) {
            const char* found = spa_dict_lookup(props, key);
            return std::string(found ? found : "");
        };
        NodeInfo node{.id = id,
                      .media_class = value(PW_KEY_MEDIA_CLASS),
                      .role = value(PW_KEY_MEDIA_ROLE),
                      .name = value(PW_KEY_NODE_NAME),
                      .description = value(PW_KEY_NODE_DESCRIPTION)};
        if (node.description.empty()) node.description = value(PW_KEY_NODE_NICK);
        nodes_.push_back(std::move(node));
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
    std::vector<NodeInfo> nodes_;
};

}  // namespace

Result<std::vector<NodeInfo>> list_nodes() { return NodeLister().list(); }

}  // namespace ratcast::pipewire
