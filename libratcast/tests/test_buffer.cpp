#include "ratcast/core/buffer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

using namespace ratcast;

namespace {

std::vector<uint8_t> pattern(size_t size) {
    std::vector<uint8_t> bytes(size);
    std::iota(bytes.begin(), bytes.end(), uint8_t{1});
    return bytes;
}

bool same_bytes(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    return std::ranges::equal(a, b);
}

}  // namespace

// ── MutableBuffer ───────────────────────────────────────────────────────────

TEST(MutableBuffer, StartsEmptyWithTheRoomAskedFor) {
    BufferPool pool;
    MutableBuffer buffer = MutableBuffer::allocate(1000, 64, pool);
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.headroom(), 64u);
    EXPECT_GE(buffer.tailroom(), 1000u);
}

TEST(MutableBuffer, AppendsAndResizes) {
    BufferPool pool;
    const auto bytes = pattern(300);
    MutableBuffer buffer = MutableBuffer::allocate(bytes.size(), 0, pool);
    buffer.append(std::span(bytes).first(100));
    buffer.append(std::span(bytes).subspan(100));
    EXPECT_TRUE(same_bytes(buffer.bytes(), bytes));

    buffer.resize(10);
    EXPECT_EQ(buffer.size(), 10u);
    EXPECT_TRUE(same_bytes(buffer.bytes(), std::span(bytes).first(10)));
}

TEST(MutableBuffer, PrependsInPlaceWithoutMovingTheData) {
    BufferPool pool;
    const auto payload = pattern(500);
    MutableBuffer buffer = MutableBuffer::allocate(payload.size(), 32, pool);
    buffer.append(payload);
    const uint8_t* payload_at = buffer.data();

    uint8_t* header = buffer.prepend(12);
    std::memset(header, 0xAB, 12);

    EXPECT_EQ(header, payload_at - 12);
    EXPECT_EQ(buffer.data(), header);
    EXPECT_EQ(buffer.size(), 512u);
    EXPECT_EQ(buffer.headroom(), 20u);
    EXPECT_TRUE(same_bytes(buffer.bytes().subspan(12), payload));
}

TEST(MutableBuffer, FreezeSharesTheBytesWithoutCopying) {
    BufferPool pool;
    const auto bytes = pattern(64);
    MutableBuffer buffer = MutableBuffer::allocate(bytes.size(), 0, pool);
    buffer.append(bytes);
    const uint8_t* at = buffer.data();

    const Buffer frozen = std::move(buffer).freeze();
    EXPECT_EQ(frozen.data(), at);
    EXPECT_TRUE(same_bytes(frozen.bytes(), bytes));
    EXPECT_EQ(frozen.use_count(), 1u);
    EXPECT_TRUE(buffer.empty());  // NOLINT(bugprone-use-after-move): documented state
    EXPECT_EQ(buffer.headroom(), 0u);
}

TEST(MutableBuffer, MovesOwnership) {
    BufferPool pool;
    MutableBuffer a = MutableBuffer::allocate(16, 0, pool);
    a.append(std::array<uint8_t, 3>{1, 2, 3});
    MutableBuffer b = std::move(a);
    EXPECT_EQ(b.size(), 3u);
    EXPECT_EQ(a.size(), 0u);  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(pool.stats().outstanding, 1u);
}

TEST(MutableBuffer, ZeroSizedAllocationOwnsNothing) {
    BufferPool pool;
    MutableBuffer buffer = MutableBuffer::allocate(0, 0, pool);
    EXPECT_EQ(buffer.tailroom(), 0u);
    EXPECT_EQ(pool.stats().system_allocations, 0u);
}

// ── Buffer ──────────────────────────────────────────────────────────────────

TEST(Buffer, DefaultIsEmptyAndOwnsNothing) {
    const Buffer buffer;
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.data(), nullptr);
    EXPECT_EQ(buffer.use_count(), 0u);
}

TEST(Buffer, CopyOfCopies) {
    BufferPool pool;
    auto bytes = pattern(100);
    const Buffer buffer = Buffer::copy_of(bytes, pool);
    bytes[0] = 0;
    EXPECT_EQ(buffer.data()[0], 1);
    EXPECT_EQ(buffer.size(), 100u);
}

TEST(Buffer, CopyOfNothingAllocatesNothing) {
    BufferPool pool;
    const Buffer buffer = Buffer::copy_of({}, pool);
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(pool.stats().system_allocations, 0u);
}

TEST(Buffer, CopiesAndSlicesShareStorage) {
    BufferPool pool;
    const auto bytes = pattern(200);
    const Buffer whole = Buffer::copy_of(bytes, pool);
    {
        const Buffer copy = whole;  // NOLINT(performance-unnecessary-copy-initialization)
        const Buffer middle = whole.slice(50, 100);
        const Buffer tail = whole.slice(150);

        EXPECT_EQ(whole.use_count(), 4u);
        EXPECT_EQ(copy.data(), whole.data());
        EXPECT_EQ(middle.data(), whole.data() + 50);
        EXPECT_EQ(middle.size(), 100u);
        EXPECT_EQ(tail.size(), 50u);
        EXPECT_TRUE(same_bytes(middle.bytes(), std::span(bytes).subspan(50, 100)));
    }
    EXPECT_EQ(whole.use_count(), 1u);
}

TEST(Buffer, SliceOutlivesTheBufferItCameFrom) {
    BufferPool pool;
    const auto bytes = pattern(80);
    Buffer slice;
    {
        const Buffer whole = Buffer::copy_of(bytes, pool);
        slice = whole.slice(40);
    }
    EXPECT_EQ(pool.stats().outstanding, 1u);
    EXPECT_TRUE(same_bytes(slice.bytes(), std::span(bytes).subspan(40)));
}

TEST(Buffer, AssignmentFromItsOwnSliceIsSafe) {
    BufferPool pool;
    const auto bytes = pattern(30);
    Buffer buffer = Buffer::copy_of(bytes, pool);
    buffer = buffer.slice(10, 5);
    EXPECT_EQ(buffer.size(), 5u);
    EXPECT_EQ(buffer.data()[0], bytes[10]);
    EXPECT_EQ(buffer.use_count(), 1u);

    const Buffer& self = buffer;
    buffer = self;
    EXPECT_EQ(buffer.use_count(), 1u);
    EXPECT_EQ(buffer.data()[0], bytes[10]);
}

TEST(Buffer, MoveLeavesTheSourceEmpty) {
    BufferPool pool;
    Buffer a = Buffer::copy_of(pattern(10), pool);
    const Buffer b = std::move(a);
    EXPECT_TRUE(a.empty());  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(b.use_count(), 1u);
}

// ── BufferPool ──────────────────────────────────────────────────────────────

TEST(BufferPool, SizeClassesAreAtMostAQuarterLarger) {
    const BufferPool pool;
    EXPECT_EQ(pool.block_size(1), 256u);
    EXPECT_EQ(pool.block_size(256), 256u);
    EXPECT_EQ(pool.block_size(257), 320u);
    EXPECT_EQ(pool.block_size(512), 512u);
    EXPECT_EQ(pool.block_size(513), 640u);
    EXPECT_EQ(pool.block_size(size_t{1} << 20), size_t{1} << 20);

    size_t previous = 0;
    for (size_t bytes = 1; bytes <= (size_t{16} << 20); bytes += bytes / 7 + 1) {
        const size_t block = pool.block_size(bytes);
        EXPECT_GE(block, bytes);
        EXPECT_LE(block, std::max<size_t>(256, bytes + bytes / 4)) << bytes;
        EXPECT_GE(block, previous);
        previous = block;
    }
}

TEST(BufferPool, ReusesReleasedBlocks) {
    BufferPool pool;
    const uint8_t* first = nullptr;
    {
        MutableBuffer buffer = MutableBuffer::allocate(10'000, 0, pool);
        first = buffer.data();
    }
    EXPECT_EQ(pool.stats().cached_blocks, 1u);

    MutableBuffer again = MutableBuffer::allocate(9'500, 0, pool);  // same size class
    EXPECT_EQ(again.data(), first);

    const BufferPool::Stats stats = pool.stats();
    EXPECT_EQ(stats.system_allocations, 1u);
    EXPECT_EQ(stats.reuses, 1u);
    EXPECT_EQ(stats.outstanding, 1u);
    EXPECT_EQ(stats.cached_blocks, 0u);
}

TEST(BufferPool, KeepsNoMoreThanItsCacheLimit) {
    BufferPool pool(BufferPool::Limits{.max_cached_bytes = 100'000});
    {
        std::vector<MutableBuffer> buffers;
        for (int i = 0; i < 10; ++i) buffers.push_back(MutableBuffer::allocate(30'000, 0, pool));
    }
    const BufferPool::Stats stats = pool.stats();
    EXPECT_LE(stats.cached_bytes, 100'000u);
    EXPECT_EQ(stats.cached_blocks, 3u);  // 32 KiB blocks
    EXPECT_EQ(stats.outstanding, 0u);
}

TEST(BufferPool, DoesNotCacheBlocksPastThePooledRange) {
    BufferPool pool(BufferPool::Limits{.max_pooled_block = 64 * 1024});
    { MutableBuffer big = MutableBuffer::allocate(100'000, 0, pool); }
    EXPECT_EQ(pool.block_size(100'000), 100'000u);
    EXPECT_EQ(pool.stats().cached_blocks, 0u);
    EXPECT_EQ(pool.stats().system_allocations, 1u);
}

TEST(BufferPool, TrimEmptiesTheCache) {
    BufferPool pool;
    { MutableBuffer buffer = MutableBuffer::allocate(4096, 0, pool); }
    ASSERT_EQ(pool.stats().cached_blocks, 1u);
    pool.trim();
    EXPECT_EQ(pool.stats().cached_blocks, 0u);
    EXPECT_EQ(pool.stats().cached_bytes, 0u);
}

TEST(BufferPool, BuffersAreReleasedFromOtherThreads) {
    BufferPool pool;
    constexpr int kThreads = 4;
    constexpr int kBuffers = 2000;

    std::vector<Buffer> buffers;
    for (int i = 0; i < kBuffers; ++i) {
        MutableBuffer buffer =
            MutableBuffer::allocate(1024 + static_cast<size_t>(i % 7) * 512, 16, pool);
        std::memset(buffer.append(1024), i & 0xFF, 1024);
        buffers.push_back(std::move(buffer).freeze());
    }

    // Every thread holds its own copy of every buffer and drops them as it goes,
    // so the last reference to each block dies on whichever thread is last.
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([copies = buffers]() mutable {
            for (Buffer& copy : copies) {
                ASSERT_EQ(copy.size(), 1024u);
                copy = Buffer{};
            }
        });
    }
    buffers.clear();
    for (std::thread& thread : threads) thread.join();

    const BufferPool::Stats stats = pool.stats();
    EXPECT_EQ(stats.outstanding, 0u);
    EXPECT_EQ(stats.cached_blocks, stats.system_allocations);
}
