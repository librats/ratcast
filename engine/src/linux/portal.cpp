// The ScreenCast portal over GDBus. Every step of a cast is a method that
// returns a Request object at once and answers later, with a Response signal
// on that object — after the user has chosen, for Start. The signal is
// subscribed to before the call, at the path the portal will use (it is made
// from our bus name and a token we pick), so an answer cannot slip past.
//
// The wait runs a main context of our own on the calling thread; nothing here
// needs, or disturbs, a main loop the application might run.

#include "linux/portal.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "ratcast/core/function_ref.h"

#include "librats/util/logger.h"

namespace ratcast::portal {
namespace {

constexpr const char* kBusName = "org.freedesktop.portal.Desktop";
constexpr const char* kObjectPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr const char* kRequest = "org.freedesktop.portal.Request";
constexpr const char* kSession = "org.freedesktop.portal.Session";

// ScreenCast's source types, cursor modes and persist modes.
constexpr uint32_t kSourceMonitor = 1;
constexpr uint32_t kSourceWindow = 2;
constexpr uint32_t kCursorHidden = 1;
constexpr uint32_t kCursorEmbedded = 2;
constexpr uint32_t kPersistUntilRevoked = 2;

// Responses to a request.
constexpr uint32_t kResponseSuccess = 0;
constexpr uint32_t kResponseCancelled = 1;

struct VariantDeleter {
    void operator()(GVariant* variant) const noexcept { g_variant_unref(variant); }
};
using VariantPtr = std::unique_ptr<GVariant, VariantDeleter>;

/// A GError to pass to GLib, freed when done with.
class Error {
public:
    Error() = default;
    Error(const Error&) = delete;
    Error& operator=(const Error&) = delete;
    ~Error() {
        if (error_) g_error_free(error_);
    }

    GError** out() noexcept { return &error_; }
    const char* message() const noexcept { return error_ ? error_->message : "unknown error"; }

private:
    GError* error_ = nullptr;
};

/// A main context of our own, the thread's default while this lives: where
/// the connection delivers the signals we wait for.
class MainContext {
public:
    MainContext() : context_(g_main_context_new()) { g_main_context_push_thread_default(context_); }
    MainContext(const MainContext&) = delete;
    MainContext& operator=(const MainContext&) = delete;
    ~MainContext() {
        g_main_context_pop_thread_default(context_);
        g_main_context_unref(context_);
    }

    GMainContext* get() const noexcept { return context_; }

private:
    GMainContext* context_;
};

/// A fresh connection to the session bus, ours alone.
GDBusConnection* connect_session_bus() {
    Error error;
    gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, error.out());
    if (!address) {
        LOG_WARN("engine", "no session bus: " << error.message());
        return nullptr;
    }
    GDBusConnection* connection = g_dbus_connection_new_for_address_sync(
        address,
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, error.out());
    g_free(address);
    if (!connection) LOG_WARN("engine", "cannot connect to the session bus: " << error.message());
    return connection;
}

/// Our part of request paths: the unique bus name, ":1.42" as "1_42".
std::string sender_element(GDBusConnection* connection) {
    std::string name = g_dbus_connection_get_unique_name(connection);
    if (!name.empty() && name[0] == ':') name.erase(0, 1);
    for (char& c : name) {
        if (c == '.') c = '_';
    }
    return name;
}

/// A granted cast: the connection it lives on, its session and its node.
class Cast final : public ScreenCast {
public:
    explicit Cast(GDBusConnection* connection) : connection_(connection) {}
    Cast(const Cast&) = delete;
    Cast& operator=(const Cast&) = delete;

    ~Cast() override {
        if (fd_ >= 0) close(fd_);
        if (!session_.empty()) {
            Error error;
            VariantPtr reply(g_dbus_connection_call_sync(
                connection_, kBusName, session_.c_str(), kSession, "Close", nullptr, nullptr,
                G_DBUS_CALL_FLAGS_NONE, -1, nullptr, error.out()));
            if (!reply) LOG_DEBUG("engine", "closing the screen cast: " << error.message());
        }
        g_dbus_connection_close_sync(connection_, nullptr, nullptr);
        g_object_unref(connection_);
    }

    int pipewire_fd() const noexcept override { return fd_; }
    uint32_t node() const noexcept override { return node_; }
    const std::string& restore_token() const noexcept override { return restore_token_; }

    GDBusConnection* connection() const noexcept { return connection_; }
    const std::string& session() const noexcept { return session_; }

    void set_session(std::string handle) { session_ = std::move(handle); }
    void set_stream(int fd, uint32_t node, std::string restore_token) {
        fd_ = fd;
        node_ = node;
        restore_token_ = std::move(restore_token);
    }

private:
    GDBusConnection* connection_;
    std::string session_;  ///< the session's object path, once there is one
    int fd_ = -1;
    uint32_t node_ = 0;
    std::string restore_token_;
};

/// Calls to the ScreenCast portal on one connection, waiting for the answers
/// in `context` until `stop` is requested or the portal goes away.
class Portal {
public:
    Portal(GDBusConnection* connection, GMainContext* context, std::stop_token stop)
        : connection_(connection),
          context_(context),
          stop_(std::move(stop)),
          sender_(sender_element(connection)),
          wake_on_stop_(stop_, [context] { g_main_context_wakeup(context); }) {
        watch_ = g_bus_watch_name_on_connection(
            connection_, kBusName, G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr,
            [](GDBusConnection*, const gchar*, gpointer self) {
                static_cast<Portal*>(self)->vanished_ = true;
            },
            this, nullptr);
    }
    Portal(const Portal&) = delete;
    Portal& operator=(const Portal&) = delete;
    ~Portal() { g_bus_unwatch_name(watch_); }

    /// A uint32 property of the ScreenCast interface.
    std::optional<uint32_t> property(const char* name) {
        Error error;
        VariantPtr reply(g_dbus_connection_call_sync(
            connection_, kBusName, kObjectPath, "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", kScreenCast, name), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
            -1, nullptr, error.out()));
        if (!reply) {
            LOG_DEBUG("engine", "screen cast portal: no " << name << ": " << error.message());
            return std::nullopt;
        }
        GVariant* inner = nullptr;
        g_variant_get(reply.get(), "(v)", &inner);
        const VariantPtr value(inner);
        if (!g_variant_is_of_type(value.get(), G_VARIANT_TYPE_UINT32)) return std::nullopt;
        return g_variant_get_uint32(value.get());
    }

    /// Call `method`, with the parameters `make` builds around a handle token,
    /// and wait for the answer: the results, or why there are none.
    Result<VariantPtr> request(const char* method,
                               FunctionRef<GVariant*(const char* handle_token)> make) {
        const std::string token = "ratcast" + std::to_string(next_token_++);
        const std::string path = std::string(kObjectPath) + "/request/" + sender_ + "/" + token;

        struct Response {
            std::optional<uint32_t> code;
            VariantPtr results;
        } response;
        const guint subscription = g_dbus_connection_signal_subscribe(
            connection_, kBusName, kRequest, "Response", path.c_str(), nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
               GVariant* parameters, gpointer data) {
                auto& r = *static_cast<Response*>(data);
                if (r.code || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})"))) return;
                uint32_t code = 0;
                GVariant* results = nullptr;
                g_variant_get(parameters, "(u@a{sv})", &code, &results);
                r.code = code;
                r.results.reset(results);
            },
            &response, nullptr);

        Error error;
        const VariantPtr handle(g_dbus_connection_call_sync(
            connection_, kBusName, kObjectPath, kScreenCast, method, make(token.c_str()),
            G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, error.out()));
        if (handle) {
            while (!response.code && !stop_.stop_requested() && !vanished_) {
                g_main_context_iteration(context_, TRUE);
            }
        }
        g_dbus_connection_signal_unsubscribe(connection_, subscription);

        if (!handle) {
            LOG_WARN("engine", "screen cast portal: " << method << " failed: " << error.message());
            return fail(Errc::Unavailable);
        }
        if (!response.code) {
            // Called off, or the portal went: take its dialog down, if any.
            VariantPtr closed(g_dbus_connection_call_sync(
                connection_, kBusName, path.c_str(), kRequest, "Close", nullptr, nullptr,
                G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr));
            return fail(vanished_ ? Errc::Unavailable : Errc::Cancelled);
        }
        if (*response.code == kResponseCancelled) return fail(Errc::Cancelled);
        if (*response.code != kResponseSuccess || !response.results) {
            LOG_WARN("engine", "screen cast portal: " << method << " refused");
            return fail(Errc::Unavailable);
        }
        return std::move(response.results);
    }

private:
    GDBusConnection* connection_;
    GMainContext* context_;
    std::stop_token stop_;
    std::string sender_;
    std::stop_callback<std::function<void()>> wake_on_stop_;
    guint watch_ = 0;
    bool vanished_ = false;
    inline static std::atomic<uint64_t> next_token_{0};
};

/// Start `builder` as an options dictionary holding the request's token.
void init_options(GVariantBuilder* builder, const char* handle_token) {
    g_variant_builder_init(builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(builder, "{sv}", "handle_token", g_variant_new_string(handle_token));
}

}  // namespace

Result<std::unique_ptr<ScreenCast>> start_screen_cast(const ScreenCastRequest& request,
                                                      std::stop_token stop) {
    if (!request.monitors && !request.windows) return fail(Errc::InvalidArgument);

    const MainContext context;
    GDBusConnection* connection = connect_session_bus();
    if (!connection) return fail(Errc::Unavailable);
    auto cast = std::make_unique<Cast>(connection);  // closes what was opened, on any return
    Portal portal(connection, context.get(), std::move(stop));

    const std::optional<uint32_t> version = portal.property("version");
    const std::optional<uint32_t> sources = portal.property("AvailableSourceTypes");
    if (!version || !sources) {
        LOG_WARN("engine", "no screen cast portal on this desktop");
        return fail(Errc::Unavailable);
    }
    const uint32_t types =
        ((request.monitors ? kSourceMonitor : 0) | (request.windows ? kSourceWindow : 0)) &
        *sources;
    if (types == 0) return fail(Errc::Unavailable);
    const uint32_t cursor_modes =
        *version >= 2 ? portal.property("AvailableCursorModes").value_or(0) : 0;
    const uint32_t cursor_mode = request.cursor ? kCursorEmbedded : kCursorHidden;

    Result<VariantPtr> created = portal.request("CreateSession", [](const char* token) {
        GVariantBuilder builder;
        init_options(&builder, token);
        g_variant_builder_add(&builder, "{sv}", "session_handle_token",
                              g_variant_new_string(token));
        return g_variant_new("(a{sv})", &builder);
    });
    if (!created) return fail(created.error());
    const char* session = nullptr;
    if (!g_variant_lookup(created->get(), "session_handle", "&s", &session) &&
        !g_variant_lookup(created->get(), "session_handle", "&o", &session)) {
        return fail(Errc::Internal);
    }
    cast->set_session(session);

    Result<VariantPtr> selected = portal.request("SelectSources", [&](const char* token) {
        GVariantBuilder builder;
        init_options(&builder, token);
        g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(types));
        g_variant_builder_add(&builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
        if ((cursor_modes & cursor_mode) != 0) {
            g_variant_builder_add(&builder, "{sv}", "cursor_mode",
                                  g_variant_new_uint32(cursor_mode));
        }
        if (*version >= 4) {
            g_variant_builder_add(&builder, "{sv}", "persist_mode",
                                  g_variant_new_uint32(kPersistUntilRevoked));
            if (!request.restore_token.empty()) {
                g_variant_builder_add(&builder, "{sv}", "restore_token",
                                      g_variant_new_string(request.restore_token.c_str()));
            }
        }
        return g_variant_new("(oa{sv})", cast->session().c_str(), &builder);
    });
    if (!selected) return fail(selected.error());

    // Where the user is asked, unless the restore token stands for them.
    Result<VariantPtr> started = portal.request("Start", [&](const char* token) {
        GVariantBuilder builder;
        init_options(&builder, token);
        return g_variant_new("(osa{sv})", cast->session().c_str(), "", &builder);
    });
    if (!started) return fail(started.error());
    const VariantPtr streams(
        g_variant_lookup_value(started->get(), "streams", G_VARIANT_TYPE("a(ua{sv})")));
    if (!streams || g_variant_n_children(streams.get()) == 0) {
        LOG_WARN("engine", "screen cast portal granted no stream");
        return fail(Errc::Unavailable);
    }
    uint32_t node = 0;
    g_variant_get_child(streams.get(), 0, "(u@a{sv})", &node, nullptr);
    const char* restore_token = "";
    g_variant_lookup(started->get(), "restore_token", "&s", &restore_token);

    GVariantBuilder empty;
    g_variant_builder_init(&empty, G_VARIANT_TYPE_VARDICT);
    GUnixFDList* fds = nullptr;
    Error error;
    const VariantPtr remote(g_dbus_connection_call_with_unix_fd_list_sync(
        connection, kBusName, kObjectPath, kScreenCast, "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", cast->session().c_str(), &empty), G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fds, nullptr, error.out()));
    if (!remote || !fds) {
        LOG_WARN("engine", "screen cast portal: no PipeWire connection: " << error.message());
        if (fds) g_object_unref(fds);
        return fail(Errc::Unavailable);
    }
    gint32 index = -1;
    g_variant_get(remote.get(), "(h)", &index);
    const int fd = g_unix_fd_list_get(fds, index, error.out());
    g_object_unref(fds);
    if (fd < 0) {
        LOG_WARN("engine", "screen cast portal: bad PipeWire connection: " << error.message());
        return fail(Errc::Unavailable);
    }

    cast->set_stream(fd, node, restore_token);
    LOG_INFO("engine", "screen cast granted: node " << node);
    return std::unique_ptr<ScreenCast>(std::move(cast));
}

}  // namespace ratcast::portal
