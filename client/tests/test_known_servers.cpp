#include "ratcast/client/known_servers.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using librats::HostEndpoint;

namespace {

class KnownServersTest : public ::testing::Test {
protected:
    void SetUp() override {
        folder_ = fs::temp_directory_path() /
                  ("ratcast_servers_" +
                   std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(folder_);
    }
    void TearDown() override { fs::remove_all(folder_); }

    std::string path() const { return (folder_ / KnownServers::kFile).string(); }

    std::string read() const {
        std::ifstream in(path());
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }
    void write(const std::string& text) const {
        fs::create_directories(folder_);
        std::ofstream(path()) << text;
    }

    fs::path folder_;
    const KnownServers::Clock::time_point t0 = KnownServers::Clock::time_point(1'800'000'000s);
};

const HostEndpoint kBuilt("builtin.example", 7700);
const HostEndpoint kFirst("192.0.2.1", 7700);
const HostEndpoint kSecond("2001:db8::2", 7701);

}  // namespace

// What it is told of is kept, and the next run starts from it, after the
// build's own.
TEST_F(KnownServersTest, KeepsWhatItIsToldOfForTheNextRun) {
    {
        KnownServers servers(path(), {kBuilt}, t0);
        EXPECT_EQ(servers.all(), std::vector{kBuilt});
        servers.told({kFirst}, t0);
        servers.told({kSecond, kBuilt}, t0 + 1s);
        EXPECT_EQ(servers.all(), (std::vector{kBuilt, kSecond, kFirst}));
    }
    EXPECT_EQ(read(),
              "[2001:db8::2]:7701 1800000001\n"
              "builtin.example:7700 1800000001\n"
              "192.0.2.1:7700 1800000000\n");
    const KnownServers next(path(), {kBuilt}, t0 + 2s);
    EXPECT_EQ(next.all(), (std::vector{kBuilt, kSecond, kFirst}));
}

// One told of again moves up; one not told of for a month is forgotten, the
// build's never.
TEST_F(KnownServersTest, ForgetsOneNotToldOfForAMonth) {
    KnownServers servers(path(), {kBuilt}, t0);
    servers.told({kFirst, kBuilt}, t0);
    servers.told({kSecond}, t0 + 1h);
    servers.told({kFirst}, t0 + 2h);
    EXPECT_EQ(servers.all(), (std::vector{kBuilt, kFirst, kSecond}));

    const KnownServers later(path(), {kBuilt}, t0 + 1h + KnownServers::kForgetAfter + 1s);
    EXPECT_EQ(later.all(), (std::vector{kBuilt, kFirst}));
    const KnownServers much_later(path(), {kBuilt}, t0 + 2h + KnownServers::kForgetAfter + 1s);
    EXPECT_EQ(much_later.all(), std::vector{kBuilt});
}

// It keeps so many, the most lately told of.
TEST_F(KnownServersTest, KeepsSoManyTheMostLatelyToldOf) {
    KnownServers servers(path(), {}, t0);
    for (size_t i = 0; i < KnownServers::kMost + 5; ++i) {
        servers.told({HostEndpoint("192.0.2." + std::to_string(i), 7700)},
                     t0 + std::chrono::seconds(i));
    }
    const std::vector<HostEndpoint> all = servers.all();
    ASSERT_EQ(all.size(), KnownServers::kMost);
    EXPECT_EQ(all.front().host, "192.0.2." + std::to_string(KnownServers::kMost + 4));
    EXPECT_EQ(all.back().host, "192.0.2.5");
}

// A line that is no server is passed over; a server twice, its later kept.
TEST_F(KnownServersTest, PassesOverWhatIsNoServer) {
    write(
        "192.0.2.1:7700 1800000000\n"
        "nonsense\n"
        "host.example:x 1800000000\n"
        "host.example:7700 soon\n"
        "\n"
        "[2001:db8::2]:7701 1800000005\n"
        "192.0.2.1:7700 1800000009\n");
    const KnownServers servers(path(), {}, t0 + 10s);
    EXPECT_EQ(servers.all(), (std::vector{kFirst, kSecond}));
}

// No file: the build's alone; no path: nothing written anywhere.
TEST_F(KnownServersTest, WithoutAFileOrAPath) {
    KnownServers servers(path(), {kBuilt}, t0);
    EXPECT_EQ(servers.all(), std::vector{kBuilt});
    KnownServers nowhere("", {}, t0);
    nowhere.told({kFirst}, t0);
    EXPECT_EQ(nowhere.all(), std::vector{kFirst});
    EXPECT_FALSE(fs::exists(path()));
}

// The build's list: RATCAST_SERVERS, each as a user writes a server.
TEST_F(KnownServersTest, TheBuildsListIsTheBuildsServers) {
    for (const HostEndpoint& at : KnownServers::built_in()) {
        EXPECT_FALSE(at.host.empty());
        EXPECT_NE(at.port, 0);
    }
}
