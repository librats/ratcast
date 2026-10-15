#pragma once

/**
 * @file buffer.h
 * @brief Media bytes: built once in a MutableBuffer, then shared as a Buffer.
 *
 * Every byte of media libratcast handles travels in one of these. The shape
 * follows the life of a frame:
 *
 *   1. An encoder writes the payload into a MutableBuffer, which owns its
 *      storage alone and keeps room free in front of the data (headroom).
 *   2. The wire layer writes the frame header into that headroom — in place,
 *      so header and payload end up contiguous without copying the payload.
 *   3. freeze() turns it into a Buffer: immutable, and shared by copying the
 *      handle (one atomic increment), never the bytes. Slices share too, so a
 *      received message and the payload cut from it are one allocation.
 *
 * Storage comes from a BufferPool that keeps released blocks for reuse, so a
 * steady stream of frames makes no trips to the system allocator at all, with a
 * bounded amount of memory held back. That does not depend on how a platform's
 * allocator treats large blocks: glibc keeps recycling them, but allocators that
 * hand them back to the OS on every free make each keyframe page-fault fresh,
 * zeroed memory (bench/bench_core.cpp measures it).
 *
 * Thread-safety: a Buffer may be copied, read and released from any thread.
 * A MutableBuffer belongs to one thread at a time. A BufferPool is
 * thread-safe, and a block may be released on a thread other than the one
 * that allocated it.
 */

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "ratcast/export.h"

namespace ratcast {

class BufferPool;

namespace detail {
struct BufferBlock;
}  // namespace detail

/// Shared, immutable bytes. A default-constructed Buffer is empty and owns
/// nothing.
class RATCAST_EXPORT Buffer {
public:
    Buffer() noexcept = default;
    Buffer(const Buffer& other) noexcept;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(const Buffer& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    ~Buffer();

    /// A new buffer holding a copy of `bytes`.
    static Buffer copy_of(std::span<const uint8_t> bytes);
    static Buffer copy_of(std::span<const uint8_t> bytes, BufferPool& pool);

    const uint8_t* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::span<const uint8_t> bytes() const noexcept { return {data_, size_}; }

    /// `length` bytes starting `offset` bytes in, sharing this buffer's
    /// storage. Precondition: offset + length <= size().
    Buffer slice(size_t offset, size_t length) const noexcept;

    /// Everything from `offset` on. Precondition: offset <= size().
    Buffer slice(size_t offset) const noexcept { return slice(offset, size_ - offset); }

    /// How many Buffers share this one's storage (0 when there is none).
    /// For tests and diagnostics; it may change concurrently.
    uint32_t use_count() const noexcept;

private:
    friend class MutableBuffer;
    Buffer(detail::BufferBlock* block, const uint8_t* data, size_t size) noexcept
        : block_(block), data_(data), size_(size) {}

    detail::BufferBlock* block_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

/// Writable bytes owned by one MutableBuffer alone, with free room before the
/// data (headroom) and after it (tailroom). Move-only.
class RATCAST_EXPORT MutableBuffer {
public:
    MutableBuffer() noexcept = default;
    MutableBuffer(MutableBuffer&& other) noexcept;
    MutableBuffer& operator=(MutableBuffer&& other) noexcept;
    MutableBuffer(const MutableBuffer&) = delete;
    MutableBuffer& operator=(const MutableBuffer&) = delete;
    ~MutableBuffer();

    /// An empty buffer with room for `capacity` bytes of data after `headroom`
    /// bytes kept free in front. The contents of new room are unspecified.
    static MutableBuffer allocate(size_t capacity, size_t headroom = 0);
    static MutableBuffer allocate(size_t capacity, size_t headroom, BufferPool& pool);

    uint8_t* data() noexcept { return data_; }
    const uint8_t* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::span<uint8_t> bytes() noexcept { return {data_, size_}; }
    std::span<const uint8_t> bytes() const noexcept { return {data_, size_}; }

    /// Free bytes in front of data().
    size_t headroom() const noexcept;
    /// Free bytes after the data.
    size_t tailroom() const noexcept;

    /// Grow the data by `count` bytes at the end and return where they start.
    /// Precondition: count <= tailroom().
    uint8_t* append(size_t count) noexcept;

    /// Copy `bytes` to the end. Precondition: bytes.size() <= tailroom().
    void append(std::span<const uint8_t> bytes) noexcept;

    /// Grow the data by `count` bytes at the front and return data(), where
    /// they now start. Precondition: count <= headroom().
    uint8_t* prepend(size_t count) noexcept;

    /// Set the size of the data, keeping its start. Precondition:
    /// size <= size() + tailroom(). Bytes gained are unspecified.
    void resize(size_t size) noexcept;

    /// Give up write access and share the data as an immutable Buffer, without
    /// copying. Leaves this MutableBuffer empty.
    Buffer freeze() && noexcept;

private:
    detail::BufferBlock* block_ = nullptr;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

/// Recycles buffer storage. Blocks come in size classes four to a power of two
/// (…, 64, 80, 96, 112, 128 KiB, …), so a block is at most 25% larger than
/// asked for and a released one fits the next request of a similar size.
///
/// A pool must outlive every buffer allocated from it. shared() is never
/// destroyed, so that holds for it by construction.
class RATCAST_EXPORT BufferPool {
public:
    struct Limits {
        /// Released bytes kept for reuse across all size classes; a block
        /// released past this goes back to the system.
        size_t max_cached_bytes = size_t{64} << 20;
        /// Largest block that is pooled at all; larger ones are allocated and
        /// freed directly.
        size_t max_pooled_block = size_t{16} << 20;
    };

    struct Stats {
        uint64_t system_allocations = 0;  ///< blocks taken from the system allocator
        uint64_t reuses = 0;              ///< blocks served from the cache instead
        uint64_t outstanding = 0;         ///< blocks currently held by buffers
        uint64_t cached_blocks = 0;       ///< blocks waiting in the cache
        uint64_t cached_bytes = 0;        ///< storage bytes waiting in the cache
    };

    BufferPool() : BufferPool(Limits{}) {}
    explicit BufferPool(Limits limits);
    ~BufferPool();
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    /// The process-wide pool used when none is given.
    static BufferPool& shared() noexcept;

    /// Storage bytes of the block a request for `bytes` gets: the smallest
    /// size class that holds it, or `bytes` itself past the pooled range.
    size_t block_size(size_t bytes) const noexcept;

    Stats stats() const;

    /// Return every cached block to the system.
    void trim() noexcept;

private:
    friend class Buffer;
    friend class MutableBuffer;
    friend struct detail::BufferBlock;

    detail::BufferBlock* acquire(size_t bytes);
    void release(detail::BufferBlock* block) noexcept;

    const Limits limits_;
    const size_t class_count_;

    mutable std::mutex mutex_;
    std::vector<detail::BufferBlock*> free_;  ///< per size class: head of an intrusive list
    Stats stats_;
};

}  // namespace ratcast
