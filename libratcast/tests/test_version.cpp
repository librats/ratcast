#include "ratcast/version.h"

#include <gtest/gtest.h>

#include <string>

TEST(Version, LinkedLibraryMatchesHeader) {
    EXPECT_EQ(std::string(ratcast::version()), RATCAST_VERSION_STRING);
}

TEST(Version, ReportsLibrats) {
    EXPECT_FALSE(std::string(ratcast::librats_version()).empty());
}
