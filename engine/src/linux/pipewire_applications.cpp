// The filter processes on PipeWire's realtime data thread (RT_PROCESS); the
// registry, the clients bound to read their process and the links are the
// loop thread's, which is where everything but process() runs.

#include "linux/pipewire_applications.h"

#include <pipewire/filter.h>
#include <spa/utils/result.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "audio/sample_clock.h"
#include "librats/util/logger.h"
#include "linux/pipewire_stream.h"

namespace ratcast::pipewire {
namespace {

/// Which of a stereo pair's sides a channel goes to.
enum class Side { Left, Right, Both, Neither };

Side side_of(std::string_view channel) {
    static constexpr std::string_view kLeft[] = {"FL",  "SL",  "RL",  "FLC", "RLC", "FLW",
                                                 "FLH", "TFL", "TSL", "TRL", "BLC"};
    static constexpr std::string_view kRight[] = {"FR",  "SR",  "RR",  "FRC", "RRC", "FRW",
                                                  "FRH", "TFR", "TSR", "TRR", "BRC"};
    if (channel == "LFE" || channel == "LFE2") return Side::Neither;
    if (std::ranges::find(kLeft, channel) != std::end(kLeft)) return Side::Left;
    if (std::ranges::find(kRight, channel) != std::end(kRight)) return Side::Right;
    return Side::Both;  // mono, centres, and whatever else is named
}

/// Whether an application's port carrying `from` feeds the filter's `to`,
/// FL or FR.
bool feeds(std::string_view from, std::string_view to) {
    const Side side = side_of(from);
    return side == Side::Both || (side != Side::Neither && (side == Side::Left) == (to == "FL"));
}

const char* prop(const spa_dict* props, const char* key) {
    return props ? spa_dict_lookup(props, key) : nullptr;
}

std::optional<int> number(const char* value) {
    if (!value) return std::nullopt;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') return std::nullopt;
    return int(parsed);
}

bool equals(const char* value, std::string_view expected) {
    return value && std::string_view(value) == expected;
}

}  // namespace

class ApplicationsCapture::Graph {
public:
    Graph(const AudioCaptureOptions& options, Clock& clock, Sink sink, EndHandler ended)
        : format_(options.format),
          excluded_(options.excluded_process != 0 ? options.excluded_process : getpid()),
          clock_(clock),
          sink_(std::move(sink)),
          ended_(std::move(ended)),
          sample_clock_(options.format.sample_rate) {}

    ~Graph() {
        if (!loop_) return;
        // As StreamThread's teardown: a filter still being set up is waited
        // for, then the loop stops and the filter goes, which stops its data
        // thread; past that nothing calls back.
        if (filter_) {
            pw_thread_loop_lock(loop_);
            for (int second = 0;
                 second < 2 && pw_filter_get_state(filter_, nullptr) == PW_FILTER_STATE_CONNECTING;
                 ++second) {
                pw_thread_loop_timed_wait(loop_, 1);
            }
            pw_thread_loop_unlock(loop_);
        }
        pw_thread_loop_stop(loop_);
        if (filter_) {
            spa_hook_remove(&filter_listener_);
            pw_filter_destroy(filter_);
        }
        for (const auto& [ends, link] : links_) pw_proxy_destroy(link);
        for (auto& [id, client] : clients_) forget(*client);
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

    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;

    Result<void> open(Duration latency) {
        init_pipewire();
        loop_ = pw_thread_loop_new("ratcast-applications", nullptr);
        if (!loop_) return fail(Errc::Internal);
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (!context_ || pw_thread_loop_start(loop_) < 0) return fail(Errc::Internal);

        pw_thread_loop_lock(loop_);
        const Result<void> connected = connect(latency);
        pw_thread_loop_unlock(loop_);
        return connected;
    }

private:
    /// A connection to PipeWire, bound to learn whose it is: the process it
    /// says it is (inside a sandbox too), or the one the daemon saw.
    struct Client {
        pw_proxy* proxy = nullptr;
        spa_hook listener{};
        std::optional<int> seen;     ///< the process the daemon saw connect
        bool known = false;          ///< its info came
        std::optional<int> process;  ///< once known; none if neither says
    };

    struct Port {
        uint32_t node = 0;
        bool output = false;
        std::string channel;
    };

    Result<void> connect(Duration latency) {
        core_ = pw_context_connect(context_, nullptr, 0);
        if (!core_) {
            LOG_WARN("engine", "cannot connect to PipeWire: " << strerror(errno));
            return fail(Errc::Unavailable);
        }
        static constexpr pw_core_events kCoreEvents = [] {
            pw_core_events events{};
            events.version = PW_VERSION_CORE_EVENTS;
            events.error = [](void* self, uint32_t id, int, int res, const char* message) {
                static_cast<Graph*>(self)->on_core_error(id, res, message);
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
                static_cast<Graph*>(self)->on_global(id, type, props);
            };
            events.global_remove = [](void* self, uint32_t id) {
                static_cast<Graph*>(self)->on_global_remove(id);
            };
            return events;
        }();
        pw_registry_add_listener(registry_, &registry_listener_, &kRegistryEvents, this);

        char node_latency[32];
        std::snprintf(node_latency, sizeof node_latency, "%zu/%u",
                      audio_length(latency, format_.sample_rate), format_.sample_rate);
        char node_rate[32];
        std::snprintf(node_rate, sizeof node_rate, "1/%u", format_.sample_rate);
        filter_ =
            pw_filter_new(core_, "ratcast-applications",
                          pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                                            "Capture", PW_KEY_NODE_LATENCY, node_latency,
                                            PW_KEY_NODE_RATE, node_rate, PW_KEY_NODE_ALWAYS_PROCESS,
                                            "true", PW_KEY_NODE_WANT_DRIVER, "true", nullptr));
        if (!filter_) return fail(Errc::Internal);
        static constexpr pw_filter_events kFilterEvents = [] {
            pw_filter_events events{};
            events.version = PW_VERSION_FILTER_EVENTS;
            events.state_changed = [](void* self, pw_filter_state, pw_filter_state state,
                                      const char* error) {
                static_cast<Graph*>(self)->on_state_changed(state, error);
            };
            events.process = [](void* self, spa_io_position* position) {
                static_cast<Graph*>(self)->on_process(position);
            };
            return events;
        }();
        pw_filter_add_listener(filter_, &filter_listener_, &kFilterEvents, this);

        // Two sides, which a mono capture takes the mean of: a stereo
        // application's left and right, or a mono one's sound twice.
        for (const char* channel : {"FL", "FR"}) {
            void* port = pw_filter_add_port(
                filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
                pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME,
                                  (std::string("input_") + channel).c_str(), PW_KEY_AUDIO_CHANNEL,
                                  channel, nullptr),
                nullptr, 0);
            if (!port) return fail(Errc::Internal);
            (left_ ? right_ : left_) = port;
        }
        if (const int error = pw_filter_connect(filter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0);
            error < 0) {
            LOG_WARN("engine", "cannot connect a PipeWire filter: " << spa_strerror(error));
            return fail(Errc::Unavailable);
        }
        return {};
    }

    // ── The loop thread ─────────────────────────────────────────────────────

    void on_global(uint32_t id, const char* type, const spa_dict* props) {
        if (!type) return;
        if (std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
            watch_client(id, number(prop(props, PW_KEY_SEC_PID)));
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const std::optional<int> client = number(prop(props, PW_KEY_CLIENT_ID));
            if (client && equals(prop(props, PW_KEY_MEDIA_CLASS), "Stream/Output/Audio")) {
                players_[id] = uint32_t(*client);
                link_all();
            }
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
            const std::optional<int> node = number(prop(props, PW_KEY_NODE_ID));
            const char* channel = prop(props, PW_KEY_AUDIO_CHANNEL);
            if (!node || !channel || equals(prop(props, PW_KEY_PORT_MONITOR), "true")) return;
            ports_[id] = Port{.node = uint32_t(*node),
                              .output = equals(prop(props, PW_KEY_PORT_DIRECTION), "out"),
                              .channel = channel};
            link_all();
        }
    }

    void on_global_remove(uint32_t id) {
        if (auto client = clients_.find(id); client != clients_.end()) {
            forget(*client->second);
            clients_.erase(client);
        }
        players_.erase(id);
        if (ports_.erase(id) == 0) return;
        for (auto link = links_.begin(); link != links_.end();) {
            if (link->first.first == id || link->first.second == id) {
                pw_proxy_destroy(link->second);
                link = links_.erase(link);
            } else {
                ++link;
            }
        }
    }

    void watch_client(uint32_t id, std::optional<int> process) {
        auto client = std::make_unique<Client>();
        client->seen = process;
        client->proxy = static_cast<pw_proxy*>(
            pw_registry_bind(registry_, id, PW_TYPE_INTERFACE_Client, PW_VERSION_CLIENT, 0));
        if (!client->proxy) return;
        static constexpr pw_client_events kClientEvents = [] {
            pw_client_events events{};
            events.version = PW_VERSION_CLIENT_EVENTS;
            events.info = [](void* self, const pw_client_info* info) {
                static_cast<Graph*>(self)->on_client_info(info);
            };
            return events;
        }();
        pw_proxy_add_object_listener(client->proxy, &client->listener, &kClientEvents, this);
        clients_[id] = std::move(client);
    }

    void on_client_info(const pw_client_info* info) {
        const auto client = clients_.find(info->id);
        if (client == clients_.end() || !(info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS)) return;
        const std::optional<int> process = number(prop(info->props, PW_KEY_APP_PROCESS_ID));
        client->second->process = process ? process : client->second->seen;
        client->second->known = true;
        link_all();
    }

    static void forget(Client& client) {
        spa_hook_remove(&client.listener);
        pw_proxy_destroy(client.proxy);
    }

    /// Whether `node` plays sound that is heard: whose it is is known, and it
    /// is not the excluded process's. (A client that says no process cannot
    /// be this one, which says.)
    bool heard(uint32_t node) const {
        const auto player = players_.find(node);
        if (player == players_.end()) return false;
        const auto client = clients_.find(player->second);
        return client != clients_.end() && client->second->known &&
               client->second->process != excluded_;
    }

    /// Every link there should be and is not yet: each heard node's output
    /// ports to the filter's inputs whose channels they feed.
    void link_all() {
        const uint32_t self = pw_filter_get_node_id(filter_);
        if (self == SPA_ID_INVALID) return;  // the ports are linked once it is there
        std::vector<std::pair<uint32_t, const Port*>> inputs;
        for (const auto& [id, port] : ports_) {
            if (port.node == self && !port.output) inputs.emplace_back(id, &port);
        }
        for (const auto& [from_id, from] : ports_) {
            if (!from.output || !heard(from.node)) continue;
            for (const auto& [to_id, to] : inputs) {
                if (!feeds(from.channel, to->channel) || links_.contains({from_id, to_id}))
                    continue;
                if (pw_proxy* link = make_link(from.node, from_id, self, to_id)) {
                    links_[{from_id, to_id}] = link;
                }
            }
        }
    }

    pw_proxy* make_link(uint32_t output_node, uint32_t output_port, uint32_t input_node,
                        uint32_t input_port) {
        pw_properties* props = pw_properties_new(nullptr, nullptr);
        pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", output_node);
        pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", output_port);
        pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", input_node);
        pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", input_port);
        auto* link = static_cast<pw_proxy*>(pw_core_create_object(
            core_, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
        pw_properties_free(props);
        return link;
    }

    void on_state_changed(pw_filter_state state, const char* error) {
        pw_thread_loop_signal(loop_, false);  // for a teardown waiting on it
        if (state == PW_FILTER_STATE_ERROR) {
            LOG_WARN("engine", "PipeWire filter failed: " << (error ? error : "unknown error"));
            finish(fail(Errc::Unavailable));
        } else if (state == PW_FILTER_STATE_PAUSED || state == PW_FILTER_STATE_STREAMING) {
            link_all();  // its node, and so its ports, are known now
        }
    }

    void on_core_error(uint32_t id, int res, const char* message) {
        // Only the connection lost ends the capture. Anything else is what
        // was there a moment ago going as it is used — a port before its
        // link is made, a client as it is bound — and costs at most one
        // application's sound.
        if (id == PW_ID_CORE && res == -EPIPE) {
            LOG_WARN("engine", "PipeWire went: " << (message ? message : ""));
            finish(fail(Errc::Unavailable));
            return;
        }
        LOG_DEBUG("engine",
                  "PipeWire: " << (message ? message : "") << " (" << spa_strerror(res) << ")");
    }

    // ── The data thread ─────────────────────────────────────────────────────

    void on_process(spa_io_position* position) {
        if (finished_.load(std::memory_order_relaxed) || !position) return;
        if (position->clock.rate.denom != format_.sample_rate) {
            LOG_WARN("engine", "the PipeWire graph runs at " << position->clock.rate.denom
                                                             << " Hz, not " << format_.sample_rate);
            finish(fail(Errc::Unsupported));
            return;
        }
        const auto length = static_cast<uint32_t>(position->clock.duration);
        if (length == 0) return;
        MutableAudioFrame block = MutableAudioFrame::allocate(format_, length);
        const std::span<float> out = block.samples();
        const auto* left = static_cast<const float*>(pw_filter_get_dsp_buffer(left_, length));
        const auto* right = static_cast<const float*>(pw_filter_get_dsp_buffer(right_, length));
        const auto sample = [](const float* side, size_t i) { return side ? side[i] : 0.0f; };
        for (size_t i = 0; i < length; ++i) {
            const float l = std::clamp(sample(left, i), -1.0f, 1.0f);
            const float r = std::clamp(sample(right, i), -1.0f, 1.0f);
            if (format_.channels == 1) {
                out[i] = (l + r) / 2;
            } else {
                out[2 * i] = l;
                out[2 * i + 1] = r;
            }
        }
        sink_(std::move(block).freeze(sample_clock_.stamp(length, clock_.now())));
    }

    /// Once, from whichever thread got there first.
    void finish(Result<void> outcome) {
        if (finished_.exchange(true)) return;
        if (ended_) ended_(outcome);
    }

    const AudioFormat format_;
    const int excluded_;  ///< the process whose sound is left out
    Clock& clock_;
    const Sink sink_;
    const EndHandler ended_;
    SampleClock sample_clock_;  ///< the data thread's
    std::atomic<bool> finished_{false};

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    pw_filter* filter_ = nullptr;
    spa_hook core_listener_{};
    spa_hook registry_listener_{};
    spa_hook filter_listener_{};
    void* left_ = nullptr;  ///< the filter's ports
    void* right_ = nullptr;

    // The loop thread's.
    std::map<uint32_t, std::unique_ptr<Client>> clients_;
    std::map<uint32_t, uint32_t> players_;  ///< the nodes playing sound, to their clients
    std::map<uint32_t, Port> ports_;        ///< every port with a channel, the filter's own too
    std::map<std::pair<uint32_t, uint32_t>, pw_proxy*> links_;  ///< by output and input port
};

ApplicationsCapture::ApplicationsCapture(const AudioCaptureOptions& options, Clock& clock)
    : options_(options), clock_(clock) {}

ApplicationsCapture::~ApplicationsCapture() { stop(); }

Result<void> ApplicationsCapture::start(Sink sink, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (graph_) return fail(Errc::InvalidArgument);
    auto graph = std::make_unique<Graph>(options_, clock_, std::move(sink), std::move(ended));
    if (Result<void> opened = graph->open(options_.latency); !opened) return opened;
    graph_ = std::move(graph);
    return {};
}

void ApplicationsCapture::stop() {
    std::lock_guard lock(mutex_);
    graph_.reset();
}

}  // namespace ratcast::pipewire
