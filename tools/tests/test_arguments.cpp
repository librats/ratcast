#include "common/arguments.h"

#include <gtest/gtest.h>

#include <vector>

#include "common/tool.h"

using namespace ratcast;
using namespace ratcast::tools;

namespace {

Arguments parse(std::vector<const char*> argv) {
    argv.insert(argv.begin(), "tool");
    return Arguments(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST(Arguments, ValuesFlagsAndNumbers) {
    Arguments args = parse({"--source", "file", "--file=/tmp/a b.mkv", "--once", "--fps", "30"});
    EXPECT_EQ(args.text("source"), "file");
    EXPECT_EQ(args.text("file"), "/tmp/a b.mkv");
    EXPECT_TRUE(args.flag("once"));
    EXPECT_FALSE(args.flag("headless"));
    EXPECT_EQ(args.number("fps", 1, 240), 30u);
    EXPECT_EQ(args.text("missing"), std::nullopt);
    EXPECT_TRUE(args.errors().empty());
}

TEST(Arguments, EveryMistakeIsReported) {
    Arguments args =
        parse({"stray", "--fps", "1000", "--port", "x1", "--typo", "--once", "now", "--codec"});
    EXPECT_EQ(args.number("fps", 1, 240), std::nullopt);     // out of range
    EXPECT_EQ(args.number("port", 0, 65535), std::nullopt);  // not a number
    EXPECT_TRUE(args.flag("once"));                          // but given a value
    EXPECT_EQ(args.text("codec"), std::nullopt);             // no value
    // stray, fps, port, typo (never asked for), once, codec
    EXPECT_EQ(args.errors().size(), 6u);
}

TEST(Tool, ParsesEndpointsSizesAndCodecs) {
    EXPECT_EQ(parse_endpoint("example.org:7700"),
              (std::pair<std::string, uint16_t>{"example.org", 7700}));
    EXPECT_EQ(parse_endpoint("[::1]:80"), (std::pair<std::string, uint16_t>{"::1", 80}));
    EXPECT_EQ(parse_endpoint("::1:80"), std::nullopt);  // ambiguous without brackets
    EXPECT_EQ(parse_endpoint("host:0"), std::nullopt);
    EXPECT_EQ(parse_endpoint("host:70000"), std::nullopt);
    EXPECT_EQ(parse_endpoint("host"), std::nullopt);

    EXPECT_EQ(parse_size("1920x1080"), (std::pair<uint32_t, uint32_t>{1920, 1080}));
    EXPECT_EQ(parse_size("1920"), std::nullopt);
    EXPECT_EQ(parse_size("0x10"), std::nullopt);

    EXPECT_EQ(parse_codec("hevc"), Codec::H265);
    EXPECT_EQ(parse_codec("vp9"), std::nullopt);
}
