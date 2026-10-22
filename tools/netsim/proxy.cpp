#include "netsim/proxy.h"

#include <algorithm>
#include <utility>

#ifdef _WIN32
#include <timeapi.h>  // after librats' headers, which bring in windows.h
#endif

namespace ratcast::netsim {
namespace {

/// How long the thread waits at most, so that stop() is seen.
constexpr Duration kLongestWait = std::chrono::milliseconds(50);

/// Enough not to drop a burst at the rates scenarios run.
constexpr int kSocketBuffer = 4 << 20;

librats::AddressFamily family_of(const librats::Address& address) {
    return address.ip.is_v6() ? librats::AddressFamily::IPv6 : librats::AddressFamily::IPv4;
}

librats::socket_t open_socket(const librats::Address& bind_to, librats::AddressFamily family) {
    const std::string host = bind_to.ip.is_unspecified() ? "" : bind_to.ip.to_string();
    const librats::socket_t socket = librats::create_udp_socket(bind_to.port, host, family);
    if (!librats::is_valid_socket(socket)) return RATS_INVALID_SOCKET;
    librats::set_socket_nonblocking(socket);
    librats::set_socket_buffer_sizes(socket, kSocketBuffer, kSocketBuffer);
    return socket;
}

}  // namespace

std::unique_ptr<Proxy> Proxy::open(ProxyConfig config, std::string* error) {
    const auto fail = [&](std::string why) -> std::unique_ptr<Proxy> {
        if (error) *error = std::move(why);
        return nullptr;
    };
    if (!config.target.is_valid()) return fail("no target to send to");
    librats::init_socket_library();
    std::unique_ptr<Proxy> proxy(new Proxy(std::move(config)));
    proxy->poller_ = librats::IOPoller::create();
    const librats::Address& listen = proxy->config_.listen;
    proxy->listen_ = open_socket(listen, family_of(listen));
    if (proxy->listen_ == RATS_INVALID_SOCKET) {
        return fail("cannot listen on " + listen.to_string());
    }
    const int port = librats::get_bound_port(proxy->listen_);
    if (port <= 0 || !proxy->poller_ || !proxy->poller_->add(proxy->listen_, librats::PollIn)) {
        return fail("cannot poll " + listen.to_string());
    }
    proxy->port_ = static_cast<uint16_t>(port);
    return proxy;
}

Proxy::Proxy(ProxyConfig config)
    : config_(std::move(config)),
      forward_(config_.forward, config_.seed),
      backward_(config_.backward, config_.seed * 2 + 1) {}

Proxy::~Proxy() {
    for (const auto& [dialer, socket] : upstream_) librats::close_socket(socket);
    if (listen_ != RATS_INVALID_SOCKET) librats::close_socket(listen_);
}

Proxy::Stats Proxy::stats() const {
    std::lock_guard lock(mutex_);
    Stats stats = stats_;
    stats.forward = forward_.stats();
    stats.backward = backward_.stats();
    return stats;
}

void Proxy::run() {
#ifdef _WIN32
    // Waits end on the system timer's tick, 15.6 ms by default: a path's delays
    // would come that coarse. 1 ms while the proxy runs.
    timeBeginPeriod(1);
#endif
    std::vector<uint8_t> buffer(64 * 1024);
    std::vector<librats::PollResult> ready(64);
    while (!stopping_.load()) {
        Timestamp now = SteadyClock::instance().now();
        apply_changes(now);
        send_due(now);

        Duration wait = kLongestWait;
        if (!departures_.empty()) {
            wait = std::clamp(departures_.top().at - now, Duration::zero(), kLongestWait);
        }
        // Rounded up: woken early, the thread would only spin until it is due.
        const auto wait_ms = std::chrono::ceil<std::chrono::milliseconds>(wait);
        const int count = poller_->wait(ready.data(), static_cast<int>(ready.size()),
                                        static_cast<int>(wait_ms.count()));
        now = SteadyClock::instance().now();
        for (int i = 0; i < count; ++i) {
            if (ready[static_cast<size_t>(i)].events & librats::PollIn) {
                read(ready[static_cast<size_t>(i)].fd, buffer, now);
            }
        }
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

// Everything the socket holds: from a dialer on the listening socket, from the
// target on a dialer's own.
void Proxy::read(librats::socket_t socket, std::vector<uint8_t>& buffer, Timestamp now) {
    for (;;) {
        librats::Address from;
        const std::ptrdiff_t size =
            librats::recv_udp_from(socket, buffer.data(), buffer.size(), from);
        if (size < 0) return;  // nothing more, or an error the next datagram may not have
        const std::span<const uint8_t> bytes(buffer.data(), static_cast<size_t>(size));
        if (!first_) first_ = now;
        if (socket == listen_) {
            const librats::socket_t up = upstream(from);
            if (up != RATS_INVALID_SOCKET) {
                schedule(backward_, false, up, config_.target, bytes, now);
            }
        } else if (const auto it = dialer_of_.find(socket); it != dialer_of_.end()) {
            if (from == config_.target) schedule(forward_, true, listen_, it->second, bytes, now);
        }
    }
}

librats::socket_t Proxy::upstream(const librats::Address& dialer) {
    if (const auto it = upstream_.find(dialer); it != upstream_.end()) return it->second;
    const librats::socket_t socket = open_socket(librats::Address{}, family_of(config_.target));
    if (socket == RATS_INVALID_SOCKET) return RATS_INVALID_SOCKET;
    if (!poller_->add(socket, librats::PollIn)) {
        librats::close_socket(socket);
        return RATS_INVALID_SOCKET;
    }
    upstream_.emplace(dialer, socket);
    dialer_of_.emplace(socket, dialer);
    std::lock_guard lock(mutex_);
    stats_.dialers = upstream_.size();
    return socket;
}

void Proxy::schedule(Link& link, bool forward, librats::socket_t socket, const librats::Address& to,
                     std::span<const uint8_t> bytes, Timestamp now) {
    std::optional<Timestamp> leaves;
    {
        std::lock_guard lock(mutex_);
        leaves = link.enter(bytes.size(), now);
    }
    if (!leaves) return;
    departures_.push({*leaves, order_++, socket, to, {bytes.begin(), bytes.end()}, forward});
}

void Proxy::send_due(Timestamp now) {
    while (!departures_.empty() && departures_.top().at <= now) {
        const Departure& departure = departures_.top();
        const std::ptrdiff_t sent = librats::send_udp_to(
            departure.socket, departure.bytes.data(), departure.bytes.size(), departure.to,
            departure.socket == listen_ ? family_of(config_.listen) : family_of(config_.target));
        {
            std::lock_guard lock(mutex_);
            if (sent != static_cast<std::ptrdiff_t>(departure.bytes.size())) {
                ++stats_.send_failures;
            } else if (departure.forward) {
                ++stats_.forward_delivered;
                stats_.forward_bytes += departure.bytes.size();
            } else {
                ++stats_.backward_delivered;
                stats_.backward_bytes += departure.bytes.size();
            }
        }
        departures_.pop();
    }
}

void Proxy::apply_changes(Timestamp now) {
    while (first_ && changes_done_ < config_.changes.size() &&
           now - *first_ >= config_.changes[changes_done_].at) {
        std::lock_guard lock(mutex_);
        forward_.reconfigure(config_.changes[changes_done_++].forward);
    }
}

}  // namespace ratcast::netsim
