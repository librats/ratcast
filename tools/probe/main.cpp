// ratcast-tsprobe: a stamped MPEG-TS feed at a constant bitrate, and its meter
// — how a transport of TS feeds, ratcast or SRT or librist, carries one. The
// sender feeds the transport's input, the meter reads its output; each prints
// a line a second and, at the end, a summary line of key=value pairs.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/arguments.h"
#include "common/tool.h"
#include "ratcast/client/cpu_meter.h"
#include "ratcast/client/ts_probe.h"
#include "ratcast/engine/ts_io.h"

using namespace ratcast;
using namespace ratcast::tools;
using namespace std::chrono_literals;

namespace {

constexpr const char* kUsage = R"(usage: ratcast-tsprobe send URL [options]
       ratcast-tsprobe meter URL [options]

  send URL             send the probe to URL: udp://host:port, - for standard output
  meter URL            measure the probe read from URL: udp://[host]:port (a multicast
                       group is joined), - for standard input
  --bitrate KBPS       the probe's rate (default 8000)
  --duration SECONDS   stop after this long (default: at Ctrl-C)

The probe is stamped with the time of day: across two hosts the latency reads
true only if their clocks are synchronised (NTP, PTP).
)";

/// Packets to a write: a UDP datagram of them, as encoders send.
constexpr size_t kPacketsPerWrite = 7;

struct Options {
    bool send = false;
    std::string url;
    uint32_t bitrate_kbps = 8000;
    std::optional<std::chrono::seconds> duration;
};

std::optional<Options> parse(int argc, char** argv) {
    Arguments args(argc, argv);
    Options o;
    if (std::optional<uint32_t> kbps = args.number("bitrate", 100, 1'000'000))
        o.bitrate_kbps = *kbps;
    if (std::optional<uint32_t> seconds = args.number("duration", 1, 86'400)) {
        o.duration = std::chrono::seconds(*seconds);
    }
    const std::vector<std::string> positional = args.positional();
    std::vector<std::string> errors = args.errors();
    if (positional.size() != 2 || (positional[0] != "send" && positional[0] != "meter")) {
        errors.emplace_back("send URL or meter URL, please");
    } else {
        o.send = positional[0] == "send";
        o.url = positional[1];
    }
    for (const std::string& error : errors) std::cerr << "ratcast-tsprobe: " << error << "\n";
    if (!errors.empty()) return std::nullopt;
    return o;
}

double ms(Duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

double mbps(uint64_t bytes, Duration over) {
    return over > Duration{} ? double(bytes) * 8 / double(over.count()) : 0;
}

/// Runs `each_second(n)` once a second until the duration is up, Ctrl-C, or
/// `ended`; returns the time it ran.
template <typename EachSecond>
Duration run(const Options& o, const std::atomic<bool>& ended, EachSecond each_second) {
    const auto start = std::chrono::steady_clock::now();
    for (unsigned n = 1; !interrupted() && !ended; ++n) {
        const auto second = start + std::chrono::seconds(n);
        while (std::chrono::steady_clock::now() < second && !interrupted() && !ended) {
            std::this_thread::sleep_for(20ms);
        }
        if (interrupted() || ended) break;
        each_second(n);
        if (o.duration && std::chrono::seconds(n) >= *o.duration) break;
    }
    return std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - start);
}

int send(const Options& o) {
    Result<std::unique_ptr<TsOutput>> output = open_ts_output(o.url);
    if (!output) {
        std::cerr << "ratcast-tsprobe: cannot send to " << o.url << ": "
                  << to_string(output.error()) << "\n";
        return 1;
    }
    std::atomic<uint64_t> written{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<bool> ended{false};
    // Paced on the steady clock, stamped with the time of day.
    std::jthread sender([&](std::stop_token stop) {
        std::vector<uint8_t> packets(kPacketsPerWrite * kTsPacketSize);
        const auto every = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(double(packets.size()) * 8 / (o.bitrate_kbps * 1000.0)));
        TsProbeWriter writer;
        auto next = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            std::this_thread::sleep_until(next);
            writer.fill(packets, WallClock::instance().now());
            if (!(*output)->write(packets)) ++errors;
            written = writer.written();
            next += every;
        }
    });
    CpuMeter cpu;
    uint64_t last = 0;
    const Duration took = run(o, ended, [&](unsigned n) {
        const uint64_t now = written.load();
        std::printf("[%us] %.2f Mbit/s  %llu packets  cpu %.0f%%\n", n,
                    mbps((now - last) * kTsPacketSize, 1s),
                    static_cast<unsigned long long>(now - last), cpu.take_interval());
        std::fflush(stdout);
        last = now;
    });
    sender.request_stop();
    sender.join();
    std::printf("summary packets=%llu mbps=%.3f write_errors=%llu cpu=%.0f\n",
                static_cast<unsigned long long>(written.load()),
                mbps(written.load() * kTsPacketSize, took),
                static_cast<unsigned long long>(errors.load()), cpu.total());
    return 0;
}

int meter(const Options& o) {
    Clock& clock = WallClock::instance();
    Result<std::unique_ptr<TsInput>> input = open_ts_input(o.url, clock);
    if (!input) {
        std::cerr << "ratcast-tsprobe: cannot read " << o.url << ": " << to_string(input.error())
                  << "\n";
        return 1;
    }
    TsProbeMeter meter(clock);
    meter.start();
    const Timestamp started = clock.now();
    std::atomic<bool> ended{false};
    if (Result<void> started =
            (*input)->start([&](std::span<const uint8_t> bytes,
                                Timestamp received) { meter.take(bytes, received); },
                            [&](Result<void>) { ended = true; }, Duration{});
        !started) {
        std::cerr << "ratcast-tsprobe: " << to_string(started.error()) << "\n";
        return 1;
    }
    CpuMeter cpu;
    run(o, ended, [&](unsigned n) {
        const TsProbeStats s = meter.take_interval();
        std::printf(
            "[%us] %.2f Mbit/s  %llu packets  latency p50 %.1f p99 %.1f ms  gap %.0f ms  "
            "copies %llu  out of order %llu  cc %llu  cpu %.0f%%\n",
            n, mbps(s.bytes, 1s), static_cast<unsigned long long>(s.packets),
            ms(s.latency.percentile(0.5)), ms(s.latency.percentile(0.99)), ms(s.longest_gap),
            static_cast<unsigned long long>(s.duplicates),
            static_cast<unsigned long long>(s.reordered),
            static_cast<unsigned long long>(s.discontinuities), cpu.take_interval());
        std::fflush(stdout);
    });
    (*input)->stop();

    const TsProbeStats s = meter.total();
    const auto count = [](uint64_t n) { return static_cast<unsigned long long>(n); };
    std::printf(
        "summary packets=%llu lost=%llu loss_pct=%.3f duplicates=%llu reordered=%llu "
        "discontinuities=%llu foreign=%llu",
        count(s.packets), count(s.lost),
        s.packets + s.lost > 0 ? 100.0 * double(s.lost) / double(s.packets + s.lost) : 0,
        count(s.duplicates), count(s.reordered), count(s.discontinuities), count(s.foreign));
    if (s.first_packet) {
        // The rate over the time the feed came, from its first packet on.
        std::printf(
            " first_packet_ms=%.0f mbps=%.3f latency_p50_ms=%.1f latency_p95_ms=%.1f "
            "latency_p99_ms=%.1f latency_max_ms=%.1f longest_gap_ms=%.0f",
            ms(*s.first_packet), mbps(s.bytes, clock.now() - (started + *s.first_packet)),
            ms(s.latency.percentile(0.5)), ms(s.latency.percentile(0.95)),
            ms(s.latency.percentile(0.99)), ms(s.latency.max()), ms(s.longest_gap));
    }
    std::printf(" cpu=%.0f\n", cpu.total());
    return s.packets > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::optional<Options> options = parse(argc, argv);
    if (!options) {
        std::cerr << "\n" << kUsage;
        return 2;
    }
    catch_interrupts();
    return options->send ? send(*options) : meter(*options);
}
