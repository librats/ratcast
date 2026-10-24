// SFrame's header and its sealing (session/sframe.h): the header's encoding
// byte for byte, keys apart by KID, and nothing opened that was altered.

#include "session/sframe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace ratcast;
using namespace ratcast::sframe;

namespace {

std::vector<uint8_t> header_bytes(Header header) {
    std::vector<uint8_t> out(header_size(header));
    write_header(header, out.data());
    return out;
}

std::vector<uint8_t> bytes_of(const MutableBuffer& buffer) {
    return {buffer.data(), buffer.data() + buffer.size()};
}

BaseKey key_of(uint8_t seed) {
    BaseKey key;
    for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(seed + i);
    return key;
}

const std::vector<uint8_t> kMetadata = {0x01, 0x05, 0x02, 0x2A};
const std::vector<uint8_t> kPlaintext = {'p', 'i', 'c', 't', 'u', 'r', 'e'};

}  // namespace

TEST(SFrame, AHeaderIsAsShortAsItsValues) {
    // |X|K K K|Y|C C C|: a value under 8 in the config byte itself.
    EXPECT_EQ(header_bytes({0, 0}), (std::vector<uint8_t>{0x00}));
    EXPECT_EQ(header_bytes({7, 7}), (std::vector<uint8_t>{0x77}));
    EXPECT_EQ(header_bytes({8, 0}), (std::vector<uint8_t>{0x80, 0x08}));
    EXPECT_EQ(header_bytes({1, 0x100}), (std::vector<uint8_t>{0x19, 0x01, 0x00}));
    EXPECT_EQ(header_bytes({0x1234, 0xFF}), (std::vector<uint8_t>{0x98, 0x12, 0x34, 0xFF}));
    EXPECT_EQ(header_size({UINT64_MAX, UINT64_MAX}), kMaxHeaderSize);
}

TEST(SFrame, AHeaderReadsBackAsItWasWritten) {
    for (const Header header : {Header{0, 0}, Header{7, 8}, Header{255, 256}, Header{UINT64_MAX, 1},
                                Header{3, UINT64_MAX}}) {
        const std::vector<uint8_t> bytes = header_bytes(header);
        const Result<ReadHeader> read = read_header(bytes);
        ASSERT_TRUE(read);
        EXPECT_EQ(read->header, header);
        EXPECT_EQ(read->size, bytes.size());
        for (size_t cut = 0; cut < bytes.size(); ++cut) {
            EXPECT_EQ(read_header(std::span(bytes.data(), cut)).error(), Errc::Truncated);
        }
    }
}

TEST(SFrame, WhatIsSealedOpensBehindTheSameMetadata) {
    const Key key(3, key_of(1));
    const std::vector<uint8_t> sealed = bytes_of(key.seal(5, kMetadata, kPlaintext));
    EXPECT_EQ(sealed.size(), 1 + kPlaintext.size() + kTagSize);
    EXPECT_EQ(sealed.front(), 0x35);
    EXPECT_EQ(std::search(sealed.begin(), sealed.end(), kPlaintext.begin(), kPlaintext.end()),
              sealed.end());
    const Result<ReadHeader> header = read_header(sealed);
    ASSERT_TRUE(header);
    const Result<Buffer> opened = key.open(*header, kMetadata, sealed);
    ASSERT_TRUE(opened);
    EXPECT_EQ(std::vector<uint8_t>(opened->bytes().begin(), opened->bytes().end()), kPlaintext);
}

TEST(SFrame, NothingAlteredOpens) {
    const Key key(3, key_of(1));
    const std::vector<uint8_t> sealed = bytes_of(key.seal(9, kMetadata, kPlaintext));
    const ReadHeader header = *read_header(sealed);
    for (size_t i = 0; i < sealed.size(); ++i) {
        std::vector<uint8_t> altered = sealed;
        altered[i] ^= 0x10;
        const Result<ReadHeader> read = read_header(altered);
        if (read) {
            EXPECT_FALSE(key.open(*read, kMetadata, altered)) << "byte " << i;
        }
    }
    std::vector<uint8_t> metadata = kMetadata;
    metadata[1] ^= 1;  // the keyframe flag
    EXPECT_FALSE(key.open(header, metadata, sealed));
    EXPECT_FALSE(key.open(header, kMetadata, std::span(sealed).first(sealed.size() - 1)));
}

TEST(SFrame, AnotherKeyOpensNothing) {
    const Key key(3, key_of(1));
    const std::vector<uint8_t> sealed = bytes_of(key.seal(9, kMetadata, kPlaintext));
    const ReadHeader header = *read_header(sealed);
    EXPECT_FALSE(Key(3, key_of(2)).open(header, kMetadata, sealed)) << "another base key";
    EXPECT_FALSE(Key(4, key_of(1)).open(header, kMetadata, sealed)) << "another KID";
}

TEST(SFrame, EachFrameUnderAKeyIsSealedApart) {
    const Key key(0, key_of(1));
    const std::vector<uint8_t> first = bytes_of(key.seal(1, kMetadata, kPlaintext));
    const std::vector<uint8_t> second = bytes_of(key.seal(2, kMetadata, kPlaintext));
    EXPECT_NE(std::vector(first.begin() + 1, first.end()),
              std::vector(second.begin() + 1, second.end()));
}

TEST(SFrame, RandomKeysDiffer) { EXPECT_NE(random_key(), random_key()); }
