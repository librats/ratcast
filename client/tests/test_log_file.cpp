#include "ratcast/client/log_file.h"

#include "librats/util/logger.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace ratcast;
namespace fs = std::filesystem;

namespace {

class LogFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        folder_ = fs::temp_directory_path() /
                  ("ratcast_log_" +
                   std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(folder_);
    }
    void TearDown() override { fs::remove_all(folder_); }

    std::string path(const char* name = "ratcast.log") const { return (folder_ / name).string(); }

    static std::string read(const std::string& path) {
        std::ifstream in(path);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    fs::path folder_;
};

TEST_F(LogFileTest, KeepsWhatIsLoggedWhileItLives) {
    {
        LogFile log({.path = path()});  // the folder made for it
        LOG_WARN("test", "kept");
    }
    LOG_WARN("test", "after");

    const std::string text = read(path());
    EXPECT_NE(text.find("[test] kept"), std::string::npos) << text;
    EXPECT_EQ(text.find("after"), std::string::npos) << text;
}

TEST_F(LogFileTest, StartsAnotherAsOneFillsKeepingSoMany) {
    {
        LogFile log({.path = path(), .max_size = 100, .kept = 2});
        for (int line = 0; line < 12; ++line) LOG_WARN("test", "line " << line);
    }

    EXPECT_NE(read(path()).find("line 11"), std::string::npos);
    EXPECT_TRUE(fs::exists(path("ratcast.log.1")));
    EXPECT_TRUE(fs::exists(path("ratcast.log.2")));
    EXPECT_FALSE(fs::exists(path("ratcast.log.3")));
    EXPECT_EQ(read(path()).find("line 0"), std::string::npos);
}

}  // namespace
