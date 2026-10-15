#include "wire/bytes.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace ratcast;
using namespace ratcast::wire;

namespace {

std::vector<uint8_t> encode(uint64_t value) {
    std::vector<uint8_t> out(varint_size(value));
    Writer writer(out);
    writer.varint(value);
    EXPECT_EQ(writer.position(), out.size());
    return out;
}

}  // namespace

TEST(Varint, SizesFollowSevenBitsPerByte) {
    EXPECT_EQ(varint_size(0), 1u);
    EXPECT_EQ(varint_size(127), 1u);
    EXPECT_EQ(varint_size(128), 2u);
    EXPECT_EQ(varint_size(16383), 2u);
    EXPECT_EQ(varint_size(16384), 3u);
    EXPECT_EQ(varint_size(std::numeric_limits<uint32_t>::max()), 5u);
    EXPECT_EQ(varint_size(std::numeric_limits<uint64_t>::max()), kMaxVarintSize);
}

TEST(Varint, KnownEncodings) {
    EXPECT_EQ(encode(0), (std::vector<uint8_t>{0x00}));
    EXPECT_EQ(encode(1), (std::vector<uint8_t>{0x01}));
    EXPECT_EQ(encode(127), (std::vector<uint8_t>{0x7F}));
    EXPECT_EQ(encode(128), (std::vector<uint8_t>{0x80, 0x01}));
    EXPECT_EQ(encode(300), (std::vector<uint8_t>{0xAC, 0x02}));
}

TEST(Varint, RoundTripsAtEveryWidth) {
    std::vector<uint64_t> values = {0, std::numeric_limits<uint64_t>::max()};
    for (unsigned bits = 1; bits < 64; ++bits) {
        const uint64_t edge = uint64_t{1} << bits;
        values.insert(values.end(), {edge - 1, edge, edge + 1});
    }
    for (uint64_t value : values) {
        const auto bytes = encode(value);
        Reader reader(bytes);
        EXPECT_EQ(reader.varint(), value);
        EXPECT_TRUE(reader.ok()) << value;
        EXPECT_EQ(reader.remaining(), 0u);
    }
}

TEST(Varint, TruncatedInputFails) {
    const std::array<uint8_t, 2> bytes = {0x80, 0x80};
    Reader reader(bytes);
    EXPECT_EQ(reader.varint(), 0u);
    ASSERT_FALSE(reader.ok());
    EXPECT_EQ(reader.error(), Errc::Truncated);
}

TEST(Varint, NonMinimalEncodingIsMalformed) {
    const std::array<uint8_t, 2> one_in_two_bytes = {0x81, 0x00};
    Reader reader(one_in_two_bytes);
    reader.varint();
    ASSERT_FALSE(reader.ok());
    EXPECT_EQ(reader.error(), Errc::Malformed);
}

TEST(Varint, MoreThanSixtyFourBitsIsMalformed) {
    // Ten bytes whose last carries more than the one bit left for it.
    std::array<uint8_t, 10> overflow;
    overflow.fill(0xFF);
    overflow[9] = 0x02;
    Reader too_wide(overflow);
    too_wide.varint();
    EXPECT_EQ(too_wide.error(), Errc::Malformed);

    // Eleven bytes: a continuation bit on the tenth.
    std::array<uint8_t, 11> too_long;
    too_long.fill(0x80);
    too_long[10] = 0x01;
    Reader endless(too_long);
    endless.varint();
    EXPECT_EQ(endless.error(), Errc::Malformed);
}

TEST(Reader, FirstFailureSticks) {
    const std::array<uint8_t, 1> bytes = {0x05};
    Reader reader(bytes);
    EXPECT_EQ(reader.u8(), 5);
    EXPECT_EQ(reader.u8(), 0);  // truncated
    reader.fail(Errc::Malformed);
    EXPECT_EQ(reader.varint(), 0u);
    EXPECT_EQ(reader.error(), Errc::Truncated);
}

TEST(Reader, SkipChecksBounds) {
    const std::array<uint8_t, 4> bytes = {1, 2, 3, 4};
    Reader reader(bytes);
    reader.skip(3);
    EXPECT_EQ(reader.u8(), 4);
    reader.skip(1);
    EXPECT_EQ(reader.error(), Errc::Truncated);
}
