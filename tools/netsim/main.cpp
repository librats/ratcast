// ratcast-netsim: a path between a dialer and the target it dials, emulated in
// user space — delay, jitter, loss, bursts, reordering, a rate and its queue,
// each way, as tc netem would shape them — so that ratcast (or SRT, or
// librist) can be measured across one without root, on any OS. Prints a line
// a second and, at the end, a summary line of key=value pairs.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/arguments.h"
#include "common/tool.h"
#include "netsim/proxy.h"

using namespace ratcast;
using namespace ratcast::netsim;
using namespace ratcast::tools;
using namespace std::chrono_literals;

namespace {

constexpr const char* kUsage = R"(usage: ratcast-netsim --listen HOST:PORT --to HOST:PORT [options]

  --listen HOST:PORT   where dialers send: what a viewer dials (port 0: any, printed)
  --to HOST:PORT       where it all goes on to: the sender
  --forward OPTIONS    the path from the target to its dialers, the stream's way, in
                       tc netem's words: "delay 25ms 10ms distribution normal",
                       "loss 2%", "loss gemodel 1% 25%", "reorder 2%",
                       "rate 20mbit limit 256"
  --backward OPTIONS   the path from the dialers to the target
  --changes "S OPTIONS; S OPTIONS"
                       the forward path changed S seconds after the first datagram
  --duration SECONDS   stop after this long (default: at Ctrl-C)
  --seed N             for the path's chances (default 1)

Hosts are numeric (127.0.0.1, [::1]). Each dialer gets a socket of its own toward
the target. Datagrams leave to the millisecond.
)";

struct Options {
    ProxyConfig proxy;
    std::optional<std::chrono::seconds> duration;
};

std::optional<LinkConfig> link(std::string_view options, std::string_view name,
                               std::vector<std::string>& errors) {
    std::string why;
    std::optional<LinkConfig> config = parse_link(options, &why);
    if (!config) errors.push_back(std::string(name) + ": " + why);
    return config;
}

std::optional<Options> parse(int argc, char** argv) {
    Arguments args(argc, argv);
    if (args.flag("help")) {
        std::cout << kUsage;
        std::exit(0);
    }
    Options o;
    std::vector<std::string> errors;
    const auto address = [&](std::string_view name) -> std::optional<librats::Address> {
        const std::optional<std::string> text = args.text(name);
        if (!text) {
            errors.push_back("--" + std::string(name) + " HOST:PORT, please");
            return std::nullopt;
        }
        std::optional<librats::Address> parsed = librats::Address::parse(*text);
        if (!parsed) errors.push_back("--" + std::string(name) + ": not a numeric HOST:PORT");
        return parsed;
    };
    if (const auto listen = address("listen")) o.proxy.listen = *listen;
    if (const auto to = address("to")) o.proxy.target = *to;
    if (const auto forward = link(args.text("forward").value_or(""), "--forward", errors)) {
        o.proxy.forward = *forward;
    }
    if (const auto backward = link(args.text("backward").value_or(""), "--backward", errors)) {
        o.proxy.backward = *backward;
    }
    std::string changes = args.text("changes").value_or("");
    for (size_t at = 0; at < changes.size();) {
        const size_t end = std::min(changes.find(';', at), changes.size());
        const std::string change = changes.substr(at, end - at);
        at = end + 1;
        char* rest = nullptr;
        const double seconds = std::strtod(change.c_str(), &rest);
        if (rest == change.c_str() || seconds < 0) {
            errors.push_back("--changes: each one 'SECONDS OPTIONS'");
            continue;
        }
        if (const auto forward = link(rest, "--changes", errors)) {
            o.proxy.changes.push_back(
                {std::chrono::duration_cast<Duration>(std::chrono::duration<double>(seconds)),
                 *forward});
        }
    }
    if (const auto seconds = args.number("duration", 1, 86'400)) {
        o.duration = std::chrono::seconds(*seconds);
    }
    if (const auto seed = args.number("seed", 0, UINT32_MAX)) o.proxy.seed = *seed;
    for (const std::string& error : args.errors()) errors.push_back(error);
    for (const std::string& error : errors) std::cerr << "ratcast-netsim: " << error << "\n";
    if (!errors.empty()) return std::nullopt;
    return o;
}

double mbps(uint64_t bytes, Duration over) {
    return over > Duration{} ? double(bytes) * 8 / double(over.count()) : 0;
}

/// What the link carried: payload and the headers a wire would add.
uint64_t wire_bytes(uint64_t bytes, uint64_t datagrams) { return bytes + datagrams * kHeaderBytes; }

}  // namespace

int main(int argc, char** argv) {
    std::optional<Options> options = parse(argc, argv);
    if (!options) {
        std::cerr << "\n" << kUsage;
        return 2;
    }
    catch_interrupts();
    set_verbose(false);

    std::string error;
    std::unique_ptr<Proxy> proxy = Proxy::open(options->proxy, &error);
    if (!proxy) {
        std::cerr << "ratcast-netsim: " << error << "\n";
        return 1;
    }
    librats::Address listening = options->proxy.listen;
    listening.port = proxy->port();
    std::printf("netsim: dial %s, forwarding to %s\n", listening.to_string().c_str(),
                options->proxy.target.to_string().c_str());
    std::fflush(stdout);

    std::thread runner([&] { proxy->run(); });
    CpuMeter cpu;
    Proxy::Stats last;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned n = 1; !interrupted(); ++n) {
        const auto second = start + std::chrono::seconds(n);
        while (std::chrono::steady_clock::now() < second && !interrupted()) {
            std::this_thread::sleep_for(20ms);
        }
        if (interrupted()) break;
        const Proxy::Stats s = proxy->stats();
        std::printf(
            "[%4us] -> %6.2f Mbit/s lost %llu full %llu  <- %6.2f Mbit/s lost %llu full "
            "%llu  cpu %.0f%%\n",
            n, mbps(s.forward_bytes - last.forward_bytes, 1s),
            static_cast<unsigned long long>(s.forward.lost - last.forward.lost),
            static_cast<unsigned long long>(s.forward.overflowed - last.forward.overflowed),
            mbps(s.backward_bytes - last.backward_bytes, 1s),
            static_cast<unsigned long long>(s.backward.lost - last.backward.lost),
            static_cast<unsigned long long>(s.backward.overflowed - last.backward.overflowed),
            cpu.take_interval());
        std::fflush(stdout);
        last = s;
        if (options->duration && std::chrono::seconds(n) >= *options->duration) break;
    }
    proxy->stop();
    runner.join();

    const Proxy::Stats s = proxy->stats();
    const auto count = [](uint64_t n) { return static_cast<unsigned long long>(n); };
    std::printf(
        "summary forward_packets=%llu forward_lost=%llu forward_overflowed=%llu "
        "forward_delivered=%llu forward_wire_bytes=%llu backward_packets=%llu backward_lost=%llu "
        "backward_overflowed=%llu backward_delivered=%llu backward_wire_bytes=%llu dialers=%zu "
        "send_failures=%llu cpu=%.0f\n",
        count(s.forward.packets), count(s.forward.lost), count(s.forward.overflowed),
        count(s.forward_delivered), count(wire_bytes(s.forward_bytes, s.forward_delivered)),
        count(s.backward.packets), count(s.backward.lost), count(s.backward.overflowed),
        count(s.backward_delivered), count(wire_bytes(s.backward_bytes, s.backward_delivered)),
        s.dialers, count(s.send_failures), cpu.total());
    return 0;
}
