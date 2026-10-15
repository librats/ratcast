#include "ratcast/result.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace ratcast;

namespace {

Result<int> parse_positive(int value) {
    if (value <= 0) return fail(Errc::InvalidArgument);
    return value;
}

Result<void> check(bool good) {
    if (!good) return fail(Errc::Malformed);
    return {};
}

}  // namespace

TEST(Result, HoldsAValue) {
    const Result<int> result = parse_positive(7);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 7);
    EXPECT_EQ(result.value(), 7);
}

TEST(Result, HoldsAFailure) {
    const Result<int> result = parse_positive(-1);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), Errc::InvalidArgument);
}

TEST(Result, MovesOutMoveOnlyValues) {
    Result<std::unique_ptr<int>> result = std::make_unique<int>(42);
    ASSERT_TRUE(result);
    std::unique_ptr<int> owned = *std::move(result);
    ASSERT_NE(owned, nullptr);
    EXPECT_EQ(*owned, 42);
}

TEST(Result, ArrowReachesMembers) {
    const Result<std::string> result = std::string("frame");
    EXPECT_EQ(result->size(), 5u);
}

TEST(Result, VoidCarriesOnlySuccessOrFailure) {
    EXPECT_TRUE(check(true));
    const Result<void> failed = check(false);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error(), Errc::Malformed);
}

TEST(Result, ErrorsHaveNames) {
    EXPECT_STREQ(to_string(Errc::Truncated), "truncated");
    EXPECT_STREQ(to_string(Errc::UnsupportedVersion), "unsupported version");
}
