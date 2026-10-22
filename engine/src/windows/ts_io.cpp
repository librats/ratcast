// MPEG-TS feeds on Windows: Winsock UDP sockets, standard input and output,
// files. Each input reads on a thread of its own, waiting in short steps so
// that stop() is never kept waiting long; a file is played by its PCR on a
// high-resolution waitable timer, as Sleep() steps in 15.6 ms.

#include "ratcast/engine/ts_io.h"

// In this order: winsock2.h before windows.h, mstcpip.h after both.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <mstcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
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
#include "windows/com.h"

namespace ratcast {
namespace {

using namespace std::chrono_literals;

#ifndef SIO_UDP_CONNRESET  // mstcpip.h has it only for some target versions
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

constexpr int kPollMs = 50;
/// Packets to a UDP datagram, as every TS sender sends them: 1316 bytes.
constexpr size_t kPacketsPerDatagram = 7;

/// Winsock, for the rest of the process.
bool start_winsock() {
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

/// A socket, closed when it goes.
class Socket {
public:
    explicit Socket(SOCKET socket = INVALID_SOCKET) noexcept : socket_(socket) {}
    Socket(Socket&& other) noexcept : socket_(std::exchange(other.socket_, INVALID_SOCKET)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            socket_ = std::exchange(other.socket_, INVALID_SOCKET);
        }
        return *this;
    }
    ~Socket() { reset(); }

    SOCKET get() const noexcept { return socket_; }
    explicit operator bool() const noexcept { return socket_ != INVALID_SOCKET; }
    void reset() noexcept {
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

private:
    SOCKET socket_;
};

std::wstring wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), size);
    return out;
}

struct AddrinfoDeleter {
    void operator()(addrinfo* info) const noexcept { freeaddrinfo(info); }
};
using AddrinfoPtr = std::unique_ptr<addrinfo, AddrinfoDeleter>;

Result<AddrinfoPtr> resolve(const TsUrl& url, bool passive) {
    if (!start_winsock()) return fail(Errc::Unavailable);
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
Result<Socket> bind_udp(const TsUrl& url) {
    Result<AddrinfoPtr> found = resolve(url, true);
    if (!found) return fail(found.error());
    const addrinfo* at = found->get();
    Socket socket(::socket(at->ai_family, SOCK_DGRAM, IPPROTO_UDP));
    if (!socket) return fail(Errc::Unavailable);
    const BOOL yes = TRUE;
    setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
               sizeof yes);
    const int buffer = 4 << 20;  // a burst of a high-rate feed while the reader is descheduled
    setsockopt(socket.get(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer),
               sizeof buffer);
    // A datagram once sent from here that nobody took would otherwise fail
    // the next read (ICMP's port unreachable): a feed in carries on.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(socket.get(), SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &returned,
             nullptr, nullptr);

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
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&bound), int(at->ai_addrlen)) != 0) {
        LOG_WARN("ts", "cannot bind udp port " << url.port << ": error " << WSAGetLastError());
        return fail(Errc::Unavailable);
    }
    if (group) {
        int joined = -1;
        if (at->ai_family == AF_INET) {
            ip_mreq request{};
            request.imr_multiaddr = reinterpret_cast<const sockaddr_in*>(at->ai_addr)->sin_addr;
            request.imr_interface.s_addr = htonl(INADDR_ANY);
            joined = setsockopt(socket.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                reinterpret_cast<const char*>(&request), sizeof request);
        } else {
            ipv6_mreq request{};
            request.ipv6mr_multiaddr =
                reinterpret_cast<const sockaddr_in6*>(at->ai_addr)->sin6_addr;
            joined = setsockopt(socket.get(), IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                                reinterpret_cast<const char*>(&request), sizeof request);
        }
        if (joined != 0) {
            LOG_WARN("ts", "cannot join " << url.host << ": error " << WSAGetLastError());
            return fail(Errc::Unavailable);
        }
    }
    return socket;
}

/// Writes all of `bytes` to `file`, through short writes.
Result<void> write_all(HANDLE file, std::span<const uint8_t> bytes) {
    while (!bytes.empty()) {
        DWORD written = 0;
        const DWORD size = DWORD(std::min<size_t>(bytes.size(), 1 << 30));
        if (!WriteFile(file, bytes.data(), size, &written, nullptr)) return fail(Errc::Unavailable);
        bytes = bytes.subspan(written);
    }
    return {};
}

/// Sleeps that end within a fraction of a millisecond of when asked.
class Sleeper {
public:
    Sleeper()
        : timer_(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS)) {}

    void until(std::chrono::steady_clock::time_point when) {
        const auto left = when - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) return;
        if (!timer_) {  // before Windows 10 1803: as close as Sleep() goes
            std::this_thread::sleep_for(left);
            return;
        }
        LARGE_INTEGER due;  // negative: relative, in 100 ns
        due.QuadPart = -std::max<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100, 1);
        if (SetWaitableTimer(timer_.get(), &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer_.get(), INFINITE);
    }

private:
    win::Handle timer_;
};

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

    /// Whether something waits to be read, looking for at most `ms`.
    virtual bool readable(int ms) = 0;

    bool running() const noexcept { return running_.load(std::memory_order_relaxed); }
    Clock& clock() const noexcept { return clock_; }
    Duration quiet() const noexcept { return quiet_; }

    enum class Wait { Readable, Quiet, Stopped };

    /// Wait until there is something to read, in short steps: Readable;
    /// Quiet once `quiet` has passed without (zero: never); Stopped once
    /// stopped.
    Wait wait_readable(Duration quiet) {
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
            if (readable(timeout_ms)) return Wait::Readable;
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

/// Datagrams: whatever comes, as it comes.
class UdpInput final : public ThreadedInput {
public:
    UdpInput(Socket socket, Clock& clock) noexcept
        : ThreadedInput(clock), socket_(std::move(socket)) {}
    ~UdpInput() override { stop(); }

private:
    bool readable(int ms) override {
        WSAPOLLFD p{socket_.get(), POLLRDNORM, 0};
        return WSAPoll(&p, 1, ms) > 0;
    }

    Result<void> read(const Sink& sink) override {
        std::vector<char> buffer(65536);
        bool told = true;  // whether the sink knows of the quiet since the last bytes
        for (;;) {
            const Wait wait = wait_readable(told ? Duration{} : quiet());
            if (wait == Wait::Stopped) break;
            if (wait == Wait::Quiet) {
                sink({}, clock().now());
                told = true;
                continue;
            }
            const int got = ::recv(socket_.get(), buffer.data(), int(buffer.size()), 0);
            if (got == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK || error == WSAECONNRESET || error == WSAEMSGSIZE)
                    continue;
                return fail(Errc::Unavailable);
            }
            if (got == 0) continue;  // an empty datagram
            sink({reinterpret_cast<const uint8_t*>(buffer.data()), size_t(got)}, clock().now());
            told = quiet() == Duration{};
        }
        return {};
    }

    Socket socket_;
};

/// Standard input: a pipe, or a file it was pointed at, as fast as it comes.
class StdinInput final : public ThreadedInput {
public:
    explicit StdinInput(Clock& clock) noexcept
        : ThreadedInput(clock),
          stdin_(GetStdHandle(STD_INPUT_HANDLE)),
          pipe_(GetFileType(stdin_) == FILE_TYPE_PIPE) {}
    ~StdinInput() override { stop(); }

private:
    bool readable(int ms) override {
        if (!pipe_) return true;  // a file has its bytes, or its end, at once
        // A pipe says nothing waits until it does: looked at every few ms.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        do {
            DWORD waiting = 0;
            if (!PeekNamedPipe(stdin_, nullptr, 0, nullptr, &waiting, nullptr)) return true;
            if (waiting > 0) return true;  // or its end: read() hears which
            std::this_thread::sleep_for(1ms);
        } while (running() && std::chrono::steady_clock::now() < until);
        return false;
    }

    Result<void> read(const Sink& sink) override {
        std::vector<uint8_t> buffer(64 * kTsPacketSize);
        bool told = true;
        for (;;) {
            const Wait wait = wait_readable(told ? Duration{} : quiet());
            if (wait == Wait::Stopped) break;
            if (wait == Wait::Quiet) {
                sink({}, clock().now());
                told = true;
                continue;
            }
            DWORD got = 0;
            if (!ReadFile(stdin_, buffer.data(), DWORD(buffer.size()), &got, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) return {};  // the end of the pipe
                return fail(Errc::Unavailable);
            }
            if (got == 0) return {};  // the end of the file
            sink({buffer.data(), got}, clock().now());
            told = quiet() == Duration{};
        }
        return {};
    }

    const HANDLE stdin_;  ///< the process's, never closed here
    const bool pipe_;
};

/// A recording, played in real time by its PCR, seven packets at a time.
class FileInput final : public ThreadedInput {
public:
    FileInput(win::Handle file, Clock& clock) noexcept
        : ThreadedInput(clock), file_(std::move(file)) {}
    ~FileInput() override { stop(); }

private:
    bool readable(int) override { return true; }

    Result<void> read(const Sink& sink) override {
        using std::chrono::steady_clock;
        constexpr size_t kBlock = kPacketsPerDatagram * kTsPacketSize;
        PcrPacer pacer;
        Sleeper sleeper;
        std::array<uint8_t, kBlock> block{};
        const steady_clock::time_point start = steady_clock::now();
        std::optional<steady_clock::time_point> quiet_at;  // until the sink is told
        while (running()) {
            size_t filled = 0;
            while (filled < kBlock) {
                DWORD got = 0;
                if (!ReadFile(file_.get(), block.data() + filled, DWORD(kBlock - filled), &got,
                              nullptr)) {
                    return fail(Errc::Unavailable);
                }
                if (got == 0) break;
                filled += got;
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
            for (;;) {
                // Told even when a sleep overran the block's time, which a
                // busy machine's may: the feed did go quiet that long.
                const steady_clock::time_point now = steady_clock::now();
                if (quiet_at && now >= *quiet_at) {
                    sink({}, clock().now());
                    quiet_at.reset();
                }
                if (!running() || now >= when) break;
                sleeper.until(std::min({when, quiet_at.value_or(when), now + 5ms}));
            }
            if (!running()) return {};
            // Quiet from when the block went, however long the sink kept it.
            const steady_clock::time_point sent = steady_clock::now();
            sink({block.data(), packets * kTsPacketSize}, clock().now());
            if (quiet() > Duration{}) {
                quiet_at = sent + std::chrono::duration_cast<steady_clock::duration>(quiet());
            }
        }
        return {};
    }

    win::Handle file_;
};

// ── Outputs ─────────────────────────────────────────────────────────────────

/// Datagrams of up to seven packets to one address.
class UdpOutput final : public TsOutput {
public:
    explicit UdpOutput(Socket socket) noexcept : socket_(std::move(socket)) {}

    Result<void> write(std::span<const uint8_t> bytes) override {
        constexpr size_t kDatagram = kPacketsPerDatagram * kTsPacketSize;
        while (!bytes.empty()) {
            const size_t size = std::min(bytes.size(), kDatagram);
            const int sent =
                ::send(socket_.get(), reinterpret_cast<const char*>(bytes.data()), int(size), 0);
            // Refused: nobody listened when an earlier datagram came; a feed
            // out carries on through that.
            if (sent == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (error != WSAECONNREFUSED && error != WSAECONNRESET)
                    return fail(Errc::Unavailable);
            }
            bytes = bytes.subspan(size);
        }
        return {};
    }

private:
    Socket socket_;
};

/// A pipe or a file: every byte, in order.
class FileOutput final : public TsOutput {
public:
    /// `owned`: closed when this goes (not standard output).
    FileOutput(HANDLE file, bool owned) noexcept : file_(file), owned_(owned ? file : nullptr) {}
    Result<void> write(std::span<const uint8_t> bytes) override { return write_all(file_, bytes); }

private:
    const HANDLE file_;
    win::Handle owned_;
};

}  // namespace

Result<std::unique_ptr<TsInput>> open_ts_input(std::string_view url, Clock& clock) {
    const std::optional<TsUrl> parsed = parse_ts_url(url);
    if (!parsed) return fail(Errc::InvalidArgument);
    switch (parsed->kind) {
        case TsUrl::Kind::Udp: {
            Result<Socket> socket = bind_udp(*parsed);
            if (!socket) return fail(socket.error());
            return std::unique_ptr<TsInput>(std::make_unique<UdpInput>(std::move(*socket), clock));
        }
        case TsUrl::Kind::Stdio:
            return std::unique_ptr<TsInput>(std::make_unique<StdinInput>(clock));
        case TsUrl::Kind::File: {
            win::Handle file(CreateFileW(wide(parsed->path).c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                                         nullptr));
            if (!file) return fail(Errc::Unavailable);
            return std::unique_ptr<TsInput>(std::make_unique<FileInput>(std::move(file), clock));
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
            Socket socket(::socket(at->ai_family, SOCK_DGRAM, IPPROTO_UDP));
            if (!socket || ::connect(socket.get(), at->ai_addr, int(at->ai_addrlen)) != 0) {
                return fail(Errc::Unavailable);
            }
            return std::unique_ptr<TsOutput>(std::make_unique<UdpOutput>(std::move(socket)));
        }
        case TsUrl::Kind::Stdio:
            return std::unique_ptr<TsOutput>(
                std::make_unique<FileOutput>(GetStdHandle(STD_OUTPUT_HANDLE), false));
        case TsUrl::Kind::File: {
            const HANDLE file =
                CreateFileW(wide(parsed->path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return fail(Errc::Unavailable);
            return std::unique_ptr<TsOutput>(std::make_unique<FileOutput>(file, true));
        }
    }
    return fail(Errc::InvalidArgument);
}

}  // namespace ratcast
