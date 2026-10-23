#include "fake_portal.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string_view>

namespace ratcast::test {
namespace {

constexpr const char* kBusName = "org.freedesktop.portal.Desktop";
constexpr const char* kObjectPath = "/org/freedesktop/portal/desktop";

constexpr const char* kIntrospection = R"(
<node>
  <interface name='org.freedesktop.portal.ScreenCast'>
    <method name='CreateSession'>
      <arg type='a{sv}' direction='in'/><arg type='o' direction='out'/>
    </method>
    <method name='SelectSources'>
      <arg type='o' direction='in'/><arg type='a{sv}' direction='in'/>
      <arg type='o' direction='out'/>
    </method>
    <method name='Start'>
      <arg type='o' direction='in'/><arg type='s' direction='in'/>
      <arg type='a{sv}' direction='in'/><arg type='o' direction='out'/>
    </method>
    <method name='OpenPipeWireRemote'>
      <arg type='o' direction='in'/><arg type='a{sv}' direction='in'/>
      <arg type='h' direction='out'/>
    </method>
    <property name='AvailableSourceTypes' type='u' access='read'/>
    <property name='AvailableCursorModes' type='u' access='read'/>
    <property name='version' type='u' access='read'/>
  </interface>
  <interface name='org.freedesktop.portal.Session'>
    <method name='Close'/>
  </interface>
  <interface name='org.freedesktop.portal.Request'>
    <method name='Close'/>
  </interface>
  <interface name='org.freedesktop.portal.Inhibit'>
    <method name='Inhibit'>
      <arg type='s' direction='in'/><arg type='u' direction='in'/>
      <arg type='a{sv}' direction='in'/><arg type='o' direction='out'/>
    </method>
  </interface>
</node>)";

// The interfaces above, by their place.
constexpr int kScreenCastInterface = 0;
constexpr int kSessionInterface = 1;
constexpr int kRequestInterface = 2;
constexpr int kInhibitInterface = 3;

/// The path xdg-desktop-portal gives a caller's object: ":1.42" and "tok"
/// under `kind` become .../kind/1_42/tok.
std::string object_path(std::string_view kind, const char* sender, const char* token) {
    std::string name = sender[0] == ':' ? sender + 1 : sender;
    for (char& c : name) {
        if (c == '.') c = '_';
    }
    return std::string(kObjectPath) + "/" + std::string(kind) + "/" + name + "/" + token;
}

const char* lookup_string(GVariant* options, const char* key) {
    const char* value = "";
    g_variant_lookup(options, key, "&s", &value);
    return value;
}

}  // namespace

/// Everything that runs on the portal's own thread and main context.
class FakePortal::Service {
public:
    explicit Service(FakePortal& owner) : owner_(owner), context_(g_main_context_new()) {
        loop_ = g_main_loop_new(context_, FALSE);
        node_info_ = g_dbus_node_info_new_for_xml(kIntrospection, nullptr);
    }
    ~Service() {
        g_dbus_node_info_unref(node_info_);
        g_main_loop_unref(loop_);
        g_main_context_unref(context_);
    }

    void run(const std::string& address) {
        g_main_context_push_thread_default(context_);
        connection_ = g_dbus_connection_new_for_address_sync(
            address.c_str(),
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                              G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, nullptr, nullptr);
        if (connection_) {
            register_object(kObjectPath, kScreenCastInterface);
            if (owner_.config_.inhibits) register_object(kObjectPath, kInhibitInterface);
            owner_id_ = g_bus_own_name_on_connection(
                connection_, kBusName, G_BUS_NAME_OWNER_FLAGS_NONE,
                [](GDBusConnection*, const gchar*, gpointer self) {
                    static_cast<Service*>(self)->set_ready();
                },
                nullptr, this, nullptr);
            g_main_loop_run(loop_);
            g_bus_unown_name(owner_id_);
            g_dbus_connection_close_sync(connection_, nullptr, nullptr);
            g_object_unref(connection_);
        } else {
            set_ready();  // nothing to wait for: the constructor must return
        }
        g_main_context_pop_thread_default(context_);
    }

    void quit() { g_main_loop_quit(loop_); }

private:
    void set_ready() {
        std::lock_guard lock(owner_.mutex_);
        owner_.ready_ = true;
        owner_.changed_.notify_all();
    }

    template <typename F>
    void record(F&& update) {
        std::lock_guard lock(owner_.mutex_);
        update(owner_.seen_);
        owner_.changed_.notify_all();
    }

    void register_object(const std::string& path, int interface) {
        static constexpr GDBusInterfaceVTable kVTable = {
            [](GDBusConnection*, const gchar* sender, const gchar*, const gchar* interface,
               const gchar* method, GVariant* parameters, GDBusMethodInvocation* invocation,
               gpointer self) {
                static_cast<Service*>(self)->on_call(sender, interface, method, parameters,
                                                     invocation);
            },
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* property,
               GError**, gpointer self) -> GVariant* {
                return static_cast<Service*>(self)->on_get(property);
            },
            nullptr,
            {}};
        g_dbus_connection_register_object(connection_, path.c_str(),
                                          node_info_->interfaces[interface], &kVTable, this,
                                          nullptr, nullptr);
    }

    GVariant* on_get(std::string_view property) {
        const Config& c = owner_.config_;
        if (property == "version") return g_variant_new_uint32(c.version);
        if (property == "AvailableSourceTypes") return g_variant_new_uint32(c.source_types);
        if (property == "AvailableCursorModes") return g_variant_new_uint32(c.cursor_modes);
        return nullptr;
    }

    void on_call(const char* sender, std::string_view interface, std::string_view method,
                 GVariant* parameters, GDBusMethodInvocation* invocation) {
        if (interface == "org.freedesktop.portal.Session") {
            record([](Seen& seen) { ++seen.sessions_closed; });
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (interface == "org.freedesktop.portal.Request") {
            record([](Seen& seen) { ++seen.requests_closed; });
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }

        if (method == "OpenPipeWireRemote") {
            int pair[2];
            socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair);
            {
                std::lock_guard lock(owner_.mutex_);
                owner_.peer_fd_ = pair[1];
            }
            GUnixFDList* fds = g_unix_fd_list_new_from_array(&pair[0], 1);  // takes the fd
            g_dbus_method_invocation_return_value_with_unix_fd_list(invocation,
                                                                    g_variant_new("(h)", 0), fds);
            g_object_unref(fds);
            return;
        }

        GVariant* options =
            g_variant_get_child_value(parameters, g_variant_n_children(parameters) - 1);
        const std::string request =
            object_path("request", sender, lookup_string(options, "handle_token"));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request.c_str()));

        if (method == "Inhibit") {  // no Response: the request stands until closed
            uint32_t flags = 0;
            g_variant_get_child(parameters, 1, "u", &flags);
            const std::string reason = lookup_string(options, "reason");
            record([&](Seen& seen) {
                ++seen.inhibitions;
                seen.inhibit_flags = flags;
                seen.inhibit_reason = reason;
            });
            register_object(request, kRequestInterface);
            g_variant_unref(options);
            return;
        }

        GVariantBuilder results;
        g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
        uint32_t response = 0;
        if (method == "CreateSession") {
            const std::string session =
                object_path("session", sender, lookup_string(options, "session_handle_token"));
            register_object(session, kSessionInterface);
            g_variant_builder_add(&results, "{sv}", "session_handle",
                                  g_variant_new_string(session.c_str()));
        } else if (method == "SelectSources") {
            record([options](Seen& seen) {
                uint32_t u = 0;
                gboolean b = FALSE;
                if (g_variant_lookup(options, "types", "u", &u)) seen.types = u;
                if (g_variant_lookup(options, "multiple", "b", &b)) seen.multiple = b;
                if (g_variant_lookup(options, "cursor_mode", "u", &u)) seen.cursor_mode = u;
                if (g_variant_lookup(options, "persist_mode", "u", &u)) seen.persist_mode = u;
                const char* token = nullptr;
                if (g_variant_lookup(options, "restore_token", "&s", &token)) {
                    seen.restore_token = token;
                }
            });
        } else if (method == "Start") {
            record([](Seen& seen) { seen.started = true; });
            const Config& c = owner_.config_;
            if (!c.answer_start) {  // the dialog stays up until the caller closes it
                register_object(request, kRequestInterface);
                g_variant_builder_clear(&results);
                g_variant_unref(options);
                return;
            }
            response = c.start_response;
            GVariantBuilder streams;
            g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
            g_variant_builder_add(&streams, "(u@a{sv})", c.node,
                                  g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0));
            g_variant_builder_add(&results, "{sv}", "streams", g_variant_builder_end(&streams));
            g_variant_builder_add(&results, "{sv}", "restore_token",
                                  g_variant_new_string(c.restore_token.c_str()));
        }
        g_variant_unref(options);

        g_dbus_connection_emit_signal(connection_, sender, request.c_str(),
                                      "org.freedesktop.portal.Request", "Response",
                                      g_variant_new("(ua{sv})", response, &results), nullptr);
    }

    FakePortal& owner_;
    GMainContext* context_;
    GMainLoop* loop_;
    GDBusNodeInfo* node_info_;
    GDBusConnection* connection_ = nullptr;
    guint owner_id_ = 0;
};

FakePortal::FakePortal(const std::string& address, Config config)
    : config_(std::move(config)), service_(std::make_unique<Service>(*this)) {
    thread_ = std::thread([this, address] { service_->run(address); });
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return ready_; });
}

FakePortal::~FakePortal() {
    service_->quit();
    thread_.join();
    if (peer_fd_ >= 0) close(peer_fd_);
}

FakePortal::Seen FakePortal::seen() {
    std::lock_guard lock(mutex_);
    return seen_;
}

bool FakePortal::wait_until(bool (*done)(const Seen&)) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5), [&] { return done(seen_); });
}

bool FakePortal::wait_until_started() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5), [this] { return seen_.started; });
}

}  // namespace ratcast::test
