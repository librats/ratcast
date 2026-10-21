// MPEG-TS feeds over POSIX: UDP sockets, standard input and output, files.
// Each input reads on a thread of its own, polling in short steps so that
// stop() is never kept waiting long.

#include "ratcast/engine/ts_io.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "librats/util/logger.h"
#include "ts/pcr_pacer.h"
#include "ts/ts_url.h"

namespace ratcast {
namespace {

using namespace std::chrono_literals;

constexpr int kPollMs = 50;
/// Packets to a UDP datagram, as every TS sender sends them: 1316 bytes.
constexpr size_t kPacketsPerDatagram = 7;

/// A file descriptor, closed when it goes.
class Fd {
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    void reset() noexcept {
        if (fd_ > 2) ::close(fd_);  // never standard input or output
        fd_ = -1;
    }

private:
    int fd_;
};

struct AddrinfoDeleter {
    void operator()(addrinfo* info) const noexcept { freeaddrinfo(info); }
};
using AddrinfoPtr = std::unique_ptr<addrinfo, AddrinfoDeleter>;

Result<AddrinfoPtr> resolve(const TsUrl& url, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    addrinfo* found = nullptr;
    const std::string port = std::to_string(url.port);
    if (getaddrinfo(url.host.empty() ? nullptr : url.host.c_str(), port.c_str(), &hints, &found) !=
        0) {
        LOG_WARN("ts", "cannot resolve " << url.host << ":" << url.port);
        return fail(Errc::Unavailable);
    }
    return AddrinfoPtr(found);
}

bool is_multicast(const sockaddr* address) noexcept {
    if (address->sa_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        return (ntohl(v4->sin_addr.s_addr) >> 28) == 0xE;  // 224.0.0.0/4
    }
    if (address->sa_family == AF_INET6) {
        return reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr.s6_addr[0] == 0xFF;
    }
    return false;
}

/// A socket receiving `url`'s datagrams: bound to its port, and to its host
/// unless that is a multicast group, which it joins instead.
Result<Fd> bind_udp(const TsUrl& url) {
    Result<AddrinfoPtr> found = resolve(url, true);
    if (!found) return fail(found.error());
    const addrinfo* at = found->get();
    Fd fd(::socket(at->ai_family, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (!fd) return fail(Errc::Unavailable);
    const int one = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    const int buffer = 4 << 20;  // a burst of a high-rate feed while the reader is descheduled
    setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);

    const bool group = is_multicast(at->ai_addr);
    sockaddr_storage bound{};
    std::copy_n(reinterpret_cast<const uint8_t*>(at->ai_addr), at->ai_addrlen,
                reinterpret_cast<uint8_t*>(&bound));
    if (group) {  // any address, the group's port
        if (at->ai_family == AF_INET) {
            reinterpret_cast<sockaddr_in*>(&bound)->sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            reinterpret_cast<sockaddr_in6*>(&bound)->sin6_addr = in6addr_any;
        }
    }
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&bound), at->ai_addrlen) != 0) {
        LOG_WARN("ts", "cannot bind udp port " << url.port << ": errno " << errno);
        return fail(Errc::Unavailable);
    }
    if (group) {
        int joined = -1;
        if (at->ai_family == AF_INET) {
            ip_mreq request{};
            request.imr_multiaddr = reinterpret_cast<const sockaddr_in*>(at->ai_addr)->sin_addr;
            request.imr_interface.s_addr = htonl(INADDR_ANY);
            joined = setsockopt(fd.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof request);
        } else {
            ipv6_mreq request{};
            request.ipv6mr_multiaddr =
                reinterpret_cast<const sockaddr_in6*>(at->ai_addr)->sin6_addr;
            joined = setsockopt(fd.get(), IPPROTO_IPV6, IPV6_JOIN_GROUP, &request, sizeof request);
        }
        if (joined != 0) {
            LOG_WARN("ts", "cannot join " << url.host << ": errno " << errno);
            return fail(Errc::Unavailable);
        }
    }
    return fd;
}

/// Writes all of `bytes` to `fd`, through short writes and interruptions.
Result<void> write_all(int fd, std::span<const uint8_t> bytes) {
    while (!bytes.empty()) {
        const ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR) continue;
            return fail(Errc::Unavailable);
        }
        bytes = bytes.subspan(static_cast<size_t>(written));
    }
    return {};
}

// ── Inputs ──────────────────────────────────────────────────────────────────

/// The thread and the start/stop of every input; `read` is the input's own.
/// Each input stops in its own destructor, before its members go.
class ThreadedInput : public TsInput {
public:
    explicit ThreadedInput(Clock& clock) noexcept : clock_(clock) {}

    Result<void> start(Sink sink, EndHandler ended, Duration quiet) override {
        std::lock_guard lock(mutex_);
        if (thread_.joinable() || quiet < Duration{}) return fail(Errc::InvalidArgument);
        running_ = true;
        quiet_ = quiet;
        thread_ = std::thread([this, sink = std::move(sink), ended = std::move(ended)] {
            const Result<void> outcome = read(sink);
            if (running_.load(std::memory_order_relaxed) && ended) ended(outcome);
        });
        return {};
    }

    void stop() override {
        std::lock_guard lock(mutex_);
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

protected:
    /// Read into `sink` until the input ends (its outcome) or running() turns
    /// false (whatever, then).
    virtual Result<void> read(const Sink& sink) = 0;

    bool running() const noexcept { return running_.load(std::memory_order_relaxed); }
    Clock& clock() const noexcept { return clock_; }
    /// How long after the last bytes the sink is told the input went quiet;
    /// zero: never.
    Duration quiet() const noexcept { return quiet_; }

    enum class Wait { Readable, Quiet, Stopped };

    /// Wait until `fd` has something to read, in short steps: Readable; Quiet
    /// once `quiet` has passed without (zero: never); Stopped once stopped or
    /// failed.
    Wait wait_readable(int fd, Duration quiet) const {
        using std::chrono::steady_clock;
        const steady_clock::time_point quiet_at = steady_clock::now() + quiet;
        while (running()) {
            int timeout_ms = kPollMs;
            if (quiet > Duration{}) {
                const auto left =
                    std::chrono::ceil<std::chrono::milliseconds>(quiet_at - steady_clock::now());
                if (left.count() <= 0) return Wait::Quiet;
                timeout_ms = std::min(timeout_ms, static_cast<int>(left.count()));
            }
            pollfd p{fd, POLLIN, 0};
            const int ready = ::poll(&p, 1, timeout_ms);
            if (ready > 0) return Wait::Readable;
            if (ready < 0 && errno != EINTR) return Wait::Stopped;
        }
        return Wait::Stopped;
    }

private:
    Clock& clock_;
    Duration quiet_{};
    std::mutex mutex_;  ///< serialises start() and stop()
    std::thread thread_;
    std::atomic<bool> running_{false};
};

/// Datagrams, or a pipe: whatever comes, as it comes.
class StreamInput final : public ThreadedInput {
public:
    StreamInput(Fd fd, bool datagrams, Clock& clock) noexcept
        : ThreadedInput(clock), fd_(std::move(fd)), datagrams_(datagrams) {}
    ~StreamInput() override { stop(); }

private:
    Result<void> read(const Sink& sink) override {
        std::vector<uint8_t> buffer(datagrams_ ? 65536 : 64 * kTsPacketSize);
        bool told = true;  // whether the sink knows of the quiet since the last bytes
        for (;;) {
            const Wait wait = wait_readable(fd_.get(), told ? Duration{} : quiet());
            if (wait == Wait::Stopped) break;
            if (wait == Wait::Quiet) {
                sink({}, clock().now());
                told = true;
                continue;
            }
            const ssize_t got = ::read(fd_.get(), buffer.data(), buffer.size());
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                return fail(Errc::Unavailable);
            }
            if (got == 0) {
                if (datagrams_) continue;  // an empty datagram
                return {};                 // the end of the pipe
            }
            sink({buffer.data(), static_cast<size_t>(got)}, clock().now());
            told = quiet() == Duration{};
        }
        return {};
    }

    Fd fd_;
    const bool datagrams_;
};

/// A recording, played in real time by its PCR, seven packets at a time.
class FileInput final : public ThreadedInput {
public:
    FileInput(Fd fd, Clock& clock) noexcept : ThreadedInput(clock), fd_(std::move(fd)) {}
    ~FileInput() override { stop(); }

private:
    Result<void> read(const Sink& sink) override {
        using std::chrono::steady_clock;
        constexpr size_t kBlock = kPacketsPerDatagram * kTsPacketSize;
        PcrPacer pacer;
        std::array<uint8_t, kBlock> block{};
        const steady_clock::time_point start = steady_clock::now();
        std::optional<steady_clock::time_point> quiet_at;  // until the sink is told
        while (running()) {
            size_t filled = 0;
            while (filled < kBlock) {
                const ssize_t got = ::read(fd_.get(), block.data() + filled, kBlock - filled);
                if (got < 0 && errno == EINTR) continue;
                if (got < 0) return fail(Errc::Unavailable);
                if (got == 0) break;
                filled += static_cast<size_t>(got);
            }
            const size_t packets = filled / kTsPacketSize;
            if (packets == 0) return {};  // the end, and any partial packet with it

            // The block goes when its first packet is due.
            Duration due{};
            for (size_t i = 0; i < packets; ++i) {
                const Duration at = pacer.due(std::span<const uint8_t, kTsPacketSize>(
                    &block[i * kTsPacketSize], kTsPacketSize));
                if (i == 0) due = at;
            }
            const steady_clock::time_point when =
                start + std::chrono::duration_cast<steady_clock::duration>(due);
            while (running() && steady_clock::now() < when) {
                if (quiet_at && steady_clock::now() >= *quiet_at) {
                    sink({}, clock().now());
                    quiet_at.reset();
                }
                std::this_thread::sleep_until(
                    std::min({when, quiet_at.value_or(when), steady_clock::now() + 5ms}));
            }
            if (!running()) return {};
            sink({block.data(), packets * kTsPacketSize}, clock().now());
            if (quiet() > Duration{}) {
                quiet_at = steady_clock::now() +
                           std::chrono::duration_cast<steady_clock::duration>(quiet());
            }
        }
        return {};
    }

    Fd fd_;
};

// ── Outputs ─────────────────────────────────────────────────────────────────

/// Datagrams of up to seven packets to one address.
class UdpOutput final : public TsOutput {
public:
    explicit UdpOutput(Fd fd) noexcept : fd_(std::move(fd)) {}

    Result<void> write(std::span<const uint8_t> bytes) override {
        constexpr size_t kDatagram = kPacketsPerDatagram * kTsPacketSize;
        while (!bytes.empty()) {
            const size_t size = std::min(bytes.size(), kDatagram);
            const ssize_t sent = ::send(fd_.get(), bytes.data(), size, 0);
            if (sent < 0 && errno == EINTR) continue;
            // Refused: nobody listened when an earlier datagram came; a feed
            // out carries on through that.
            if (sent < 0 && errno != ECONNREFUSED) return fail(Errc::Unavailable);
            bytes = bytes.subspan(size);
        }
        return {};
    }

private:
    Fd fd_;
};

/// A pipe or a file: every byte, in order.
class FdOutput final : public TsOutput {
public:
    explicit FdOutput(Fd fd) noexcept : fd_(std::move(fd)) {}
    Result<void> write(std::span<const uint8_t> bytes) override {
        return write_all(fd_.get(), bytes);
    }

private:
    Fd fd_;
};

}  // namespace

Result<std::unique_ptr<TsInput>> open_ts_input(std::string_view url, Clock& clock) {
    const std::optional<TsUrl> parsed = parse_ts_url(url);
    if (!parsed) return fail(Errc::InvalidArgument);
    switch (parsed->kind) {
        case TsUrl::Kind::Udp: {
            Result<Fd> fd = bind_udp(*parsed);
            if (!fd) return fail(fd.error());
            return std::unique_ptr<TsInput>(
                std::make_unique<StreamInput>(std::move(*fd), true, clock));
        }
        case TsUrl::Kind::Stdio:
            return std::unique_ptr<TsInput>(
                std::make_unique<StreamInput>(Fd(STDIN_FILENO), false, clock));
        case TsUrl::Kind::File: {
            Fd fd(::open(parsed->path.c_str(), O_RDONLY | O_CLOEXEC));
            if (!fd) return fail(Errc::Unavailable);
            return std::unique_ptr<TsInput>(std::make_unique<FileInput>(std::move(fd), clock));
        }
    }
    return fail(Errc::InvalidArgument);
}

Result<std::unique_ptr<TsOutput>> open_ts_output(std::string_view url) {
    const std::optional<TsUrl> parsed = parse_ts_url(url);
    if (!parsed || (parsed->kind == TsUrl::Kind::Udp && parsed->host.empty())) {
        return fail(Errc::InvalidArgument);
    }
    switch (parsed->kind) {
        case TsUrl::Kind::Udp: {
            Result<AddrinfoPtr> found = resolve(*parsed, false);
            if (!found) return fail(found.error());
            const addrinfo* at = found->get();
            Fd fd(::socket(at->ai_family, SOCK_DGRAM | SOCK_CLOEXEC, 0));
            if (!fd || ::connect(fd.get(), at->ai_addr, at->ai_addrlen) != 0) {
                return fail(Errc::Unavailable);
            }
            return std::unique_ptr<TsOutput>(std::make_unique<UdpOutput>(std::move(fd)));
        }
        case TsUrl::Kind::Stdio:
            return std::unique_ptr<TsOutput>(std::make_unique<FdOutput>(Fd(STDOUT_FILENO)));
        case TsUrl::Kind::File: {
            Fd fd(::open(parsed->path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
            if (!fd) return fail(Errc::Unavailable);
            return std::unique_ptr<TsOutput>(std::make_unique<FdOutput>(std::move(fd)));
        }
    }
    return fail(Errc::InvalidArgument);
}

}  // namespace ratcast
