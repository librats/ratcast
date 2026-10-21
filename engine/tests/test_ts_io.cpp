// MPEG-TS feeds: the URLs, the PCR a recording is played by, and the bytes
// through UDP on the loopback and through files.

#include "ratcast/engine/ts_io.h"

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "ts/pcr_pacer.h"
#include "ts/ts_url.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

using Packet = std::array<uint8_t, kTsPacketSize>;

/// A packet of `pid`, its payload byte `fill`, with `pcr` (27 MHz) if given.
Packet packet(uint16_t pid, uint8_t fill, std::optional<uint64_t> pcr = std::nullopt) {
    Packet p{};
    p.fill(fill);
    p[0] = kTsSyncByte;
    p[1] = static_cast<uint8_t>(pid >> 8 & 0x1F);
    p[2] = static_cast<uint8_t>(pid);
    p[3] = 0x10;  // payload only
    if (pcr) {
        p[3] = 0x30;  // adaptation field and payload
        p[4] = 7;
        p[5] = 0x10;  // PCR flag
        const uint64_t base = *pcr / 300;
        const uint64_t extension = *pcr % 300;
        p[6] = static_cast<uint8_t>(base >> 25);
        p[7] = static_cast<uint8_t>(base >> 17);
        p[8] = static_cast<uint8_t>(base >> 9);
        p[9] = static_cast<uint8_t>(base >> 1);
        p[10] = static_cast<uint8_t>((base & 1) << 7 | 0x7E | extension >> 8);
        p[11] = static_cast<uint8_t>(extension);
    }
    return p;
}

constexpr uint64_t kTicksPerMs = 27'000;

/// A UDP port free on the loopback a moment ago.
uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address);
    socklen_t size = sizeof address;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    ::close(fd);
    return ntohs(address.sin_port);
}

/// What an input handed out, for a test to wait on.
struct Received {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<uint8_t> bytes;
    std::vector<Timestamp> at;
    bool ended = false;

    TsInput::Sink sink() {
        return [this](std::span<const uint8_t> got, Timestamp received) {
            std::lock_guard lock(mutex);
            bytes.insert(bytes.end(), got.begin(), got.end());
            at.push_back(received);
            changed.notify_all();
        };
    }
    TsInput::EndHandler end() {
        return [this](Result<void>) {
            std::lock_guard lock(mutex);
            ended = true;
            changed.notify_all();
        };
    }
    bool wait_for(size_t size, std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return bytes.size() >= size; });
    }
};

std::filesystem::path temp_path(const char* name) {
    return std::filesystem::temp_directory_path() /
           (std::string(name) + "-" + std::to_string(::getpid()) + ".ts");
}

}  // namespace

TEST(TsUrlTest, TakesUdpPipesAndFiles) {
    const std::optional<TsUrl> udp = parse_ts_url("udp://239.1.2.3:5000");
    ASSERT_TRUE(udp);
    EXPECT_EQ(udp->kind, TsUrl::Kind::Udp);
    EXPECT_EQ(udp->host, "239.1.2.3");
    EXPECT_EQ(udp->port, 5000);
    EXPECT_EQ(parse_ts_url("udp://:1234")->host, "");
    EXPECT_EQ(parse_ts_url("udp://@:1234")->port, 1234);
    EXPECT_EQ(parse_ts_url("udp://[::1]:1234?pkt_size=1316")->host, "::1");
    EXPECT_EQ(parse_ts_url("-")->kind, TsUrl::Kind::Stdio);
    EXPECT_EQ(parse_ts_url("film.ts")->path, "film.ts");
    EXPECT_FALSE(parse_ts_url(""));
    EXPECT_FALSE(parse_ts_url("udp://host"));
    EXPECT_FALSE(parse_ts_url("udp://host:0"));
    EXPECT_FALSE(parse_ts_url("udp://host:99999"));
}

TEST(TsPacketFinderTest, FindsPacketsWhateverTheReads) {
    std::vector<uint8_t> feed;
    for (uint8_t i = 0; i < 5; ++i) {
        const Packet p = packet(256, i);
        feed.insert(feed.end(), p.begin(), p.end());
        if (i == 2) feed.insert(feed.end(), {1, 2, 3});  // no packet
    }
    TsPacketFinder finder;
    std::vector<uint8_t> found;
    std::vector<size_t> runs;
    uint64_t skipped = 0;
    for (size_t at = 0; at < feed.size(); at += 100) {
        const auto read = std::span(feed).subspan(at, std::min<size_t>(100, feed.size() - at));
        skipped += finder.take(read, [&](std::span<const uint8_t> run) {
            found.insert(found.end(), run.begin(), run.end());
            runs.push_back(run.size() / kTsPacketSize);
        });
    }
    EXPECT_EQ(skipped, 3u);
    std::vector<uint8_t> expected = feed;
    expected.erase(expected.begin() + 3 * kTsPacketSize, expected.begin() + 3 * kTsPacketSize + 3);
    EXPECT_EQ(found, expected);
    EXPECT_EQ(runs, (std::vector<size_t>{1, 1, 1, 1, 1}));  // a packet whenever one is whole
}

TEST(PcrPacerTest, ReadsThePcr) {
    const uint64_t pcr = (uint64_t{1} << 33) * 300 - 1;  // the largest there is
    EXPECT_EQ(read_pcr(packet(256, 0, pcr)), pcr);
    EXPECT_EQ(read_pcr(packet(256, 0)), std::nullopt);
    EXPECT_EQ(pid_of(packet(0x1ABC, 0)), 0x1ABC);
}

TEST(PcrPacerTest, PlaysPacketsEvenlyBetweenPcrs) {
    PcrPacer pacer;
    // A PCR every 10 packets, 1 ms apart: 100 µs a packet.
    std::vector<Duration> due;
    for (int i = 0; i < 40; ++i) {
        const std::optional<uint64_t> pcr =
            i % 10 == 0 ? std::optional(1000 + i / 10 * kTicksPerMs) : std::nullopt;
        due.push_back(pacer.due(packet(256, 0, pcr)));
    }
    EXPECT_EQ(due[0], 0ms);
    EXPECT_EQ(due[5], 0ms);  // no rate yet: with the first PCR
    EXPECT_EQ(due[10], 1ms);
    EXPECT_EQ(due[15], 1500us);
    EXPECT_EQ(due[39], 3900us);
}

TEST(PcrPacerTest, FollowsOnePidAndCarriesOnThroughDiscontinuitiesAndTheWrap) {
    PcrPacer pacer;
    constexpr uint64_t kWrap = (uint64_t{1} << 33) * 300;
    EXPECT_EQ(pacer.due(packet(256, 0, kWrap - kTicksPerMs)), 0ms);
    EXPECT_EQ(pacer.due(packet(300, 0, 5)), 0ms);  // another PID's PCR: not followed
    EXPECT_EQ(pacer.due(packet(256, 0, 0)), 1ms);  // the wrap: a millisecond on
    // A cut back in time: the two packets since go on at the rate there was.
    EXPECT_EQ(pacer.due(packet(256, 0)), 1500us);
    EXPECT_EQ(pacer.due(packet(256, 0, 0)), 2ms);
    EXPECT_EQ(pacer.due(packet(256, 0, 10 * kTicksPerMs)), 12ms);
}

TEST(TsIoTest, CarriesBytesOverUdp) {
    const uint16_t port = free_port();
    const std::string url = "udp://127.0.0.1:" + std::to_string(port);
    Result<std::unique_ptr<TsInput>> input = open_ts_input(url);
    ASSERT_TRUE(input) << to_string(input.error());
    Received received;
    ASSERT_TRUE((*input)->start(received.sink(), received.end()));

    Result<std::unique_ptr<TsOutput>> output = open_ts_output(url);
    ASSERT_TRUE(output);
    std::vector<uint8_t> sent;
    for (uint8_t i = 0; i < 10; ++i) {
        const Packet p = packet(256, i);
        sent.insert(sent.end(), p.begin(), p.end());
    }
    ASSERT_TRUE((*output)->write(sent));  // ten packets: datagrams of seven and three
    ASSERT_TRUE(received.wait_for(sent.size()));
    (*input)->stop();
    EXPECT_EQ(received.bytes, sent);
    EXPECT_EQ(received.at.size(), 2u);
    EXPECT_FALSE(received.ended);

    EXPECT_FALSE(open_ts_output("udp://:1234"));  // out needs a host
    EXPECT_FALSE(open_ts_input("udp://nowhere"));
}

TEST(TsIoTest, PlaysAFileInRealTimeByItsPcr) {
    // 70 packets, a PCR every seven, 20 ms apart: ten datagrams over 180 ms.
    const std::filesystem::path path = temp_path("ratcast-pcr");
    std::vector<uint8_t> written;
    {
        std::ofstream file(path, std::ios::binary);
        for (int i = 0; i < 70; ++i) {
            const std::optional<uint64_t> pcr =
                i % 7 == 0 ? std::optional(i / 7 * 20 * kTicksPerMs) : std::nullopt;
            const Packet p = packet(256, static_cast<uint8_t>(i), pcr);
            file.write(reinterpret_cast<const char*>(p.data()), p.size());
            written.insert(written.end(), p.begin(), p.end());
        }
    }
    Result<std::unique_ptr<TsInput>> input = open_ts_input(path.string());
    ASSERT_TRUE(input);
    Received received;
    ASSERT_TRUE((*input)->start(received.sink(), received.end()));
    {
        std::unique_lock lock(received.mutex);
        ASSERT_TRUE(received.changed.wait_for(lock, 5s, [&] { return received.ended; }));
    }
    (*input)->stop();
    std::filesystem::remove(path);

    EXPECT_EQ(received.bytes, written);
    ASSERT_EQ(received.at.size(), 10u);
    const Duration took = received.at.back() - received.at.front();
    EXPECT_GE(took, 175ms);
    EXPECT_LE(took, 400ms);  // a slow machine's sleeps
}

TEST(TsIoTest, WritesAFile) {
    const std::filesystem::path path = temp_path("ratcast-out");
    {
        Result<std::unique_ptr<TsOutput>> output = open_ts_output(path.string());
        ASSERT_TRUE(output);
        const Packet p = packet(256, 7);
        ASSERT_TRUE((*output)->write(p));
        ASSERT_TRUE((*output)->write(p));
    }
    EXPECT_EQ(std::filesystem::file_size(path), 2 * kTsPacketSize);
    std::filesystem::remove(path);
    EXPECT_FALSE(open_ts_input("/no/such/file.ts"));
}
