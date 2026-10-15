#include "ratcast/buffer.h"

#include <atomic>
#include <bit>
#include <cassert>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace ratcast {
namespace detail {

/// What precedes every block's storage. The block is one allocation: this
/// header, padded to the strictest fundamental alignment, then the bytes.
struct BufferBlock {
    static constexpr uint32_t kUnpooled = std::numeric_limits<uint32_t>::max();

    std::atomic<uint32_t> refs{1};
    uint32_t size_class = kUnpooled;
    size_t capacity = 0;  ///< storage bytes after the header
    BufferPool* pool = nullptr;
    BufferBlock* next_free = nullptr;  ///< link while the block waits in its pool

    uint8_t* storage() noexcept;

    void retain() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }

    // The last reference gives the block back to its pool. acq_rel: every write
    // made through other references happens-before the pool reuses the storage.
    void unref() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) pool->release(this);
    }
};

namespace {

constexpr size_t kHeaderSize = (sizeof(BufferBlock) + alignof(std::max_align_t) - 1) /
                               alignof(std::max_align_t) * alignof(std::max_align_t);

// Size classes. Class 0 is kMinBlock. Above it every power-of-two range
// (2^k, 2^(k+1)] holds four classes, 2^k + s * 2^(k-2) for s = 1..4, so a block
// is never more than 25% larger than the request it serves.
constexpr unsigned kMinShift = 8;
constexpr size_t kMinBlock = size_t{1} << kMinShift;
constexpr size_t kClassesPerDoubling = 4;

size_t class_of(size_t bytes) noexcept {
    if (bytes <= kMinBlock) return 0;
    const unsigned k =
        static_cast<unsigned>(std::bit_width(bytes - 1)) - 1;  // 2^k < bytes <= 2^(k+1)
    const size_t step = size_t{1} << (k - 2);
    const size_t s = (bytes - (size_t{1} << k) + step - 1) / step;  // 1..4
    return 1 + (k - kMinShift) * kClassesPerDoubling + (s - 1);
}

size_t class_size(size_t index) noexcept {
    if (index == 0) return kMinBlock;
    const unsigned k = kMinShift + static_cast<unsigned>((index - 1) / kClassesPerDoubling);
    const size_t s = (index - 1) % kClassesPerDoubling + 1;
    return (size_t{1} << k) + s * (size_t{1} << (k - 2));
}

BufferBlock* new_block(BufferPool* pool, size_t capacity, uint32_t size_class) {
    void* raw = ::operator new(kHeaderSize + capacity);
    auto* block = ::new (raw) BufferBlock;
    block->size_class = size_class;
    block->capacity = capacity;
    block->pool = pool;
    return block;
}

void delete_block(BufferBlock* block) noexcept {
    block->~BufferBlock();
    ::operator delete(static_cast<void*>(block));
}

void retain(BufferBlock* block) noexcept {
    if (block) block->retain();
}

void release(BufferBlock* block) noexcept {
    if (block) block->unref();
}

}  // namespace

uint8_t* BufferBlock::storage() noexcept { return reinterpret_cast<uint8_t*>(this) + kHeaderSize; }

}  // namespace detail

using detail::BufferBlock;

// ── Buffer ──────────────────────────────────────────────────────────────────

Buffer::Buffer(const Buffer& other) noexcept
    : block_(other.block_), data_(other.data_), size_(other.size_) {
    detail::retain(block_);
}

Buffer::Buffer(Buffer&& other) noexcept
    : block_(std::exchange(other.block_, nullptr)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

Buffer& Buffer::operator=(const Buffer& other) noexcept {
    detail::retain(other.block_);  // before releasing ours: other may share our block
    detail::release(block_);
    block_ = other.block_;
    data_ = other.data_;
    size_ = other.size_;
    return *this;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        detail::release(block_);
        block_ = std::exchange(other.block_, nullptr);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

Buffer::~Buffer() { detail::release(block_); }

Buffer Buffer::copy_of(std::span<const uint8_t> bytes) {
    return copy_of(bytes, BufferPool::shared());
}

Buffer Buffer::copy_of(std::span<const uint8_t> bytes, BufferPool& pool) {
    if (bytes.empty()) return {};
    MutableBuffer copy = MutableBuffer::allocate(bytes.size(), 0, pool);
    copy.append(bytes);
    return std::move(copy).freeze();
}

Buffer Buffer::slice(size_t offset, size_t length) const noexcept {
    assert(offset <= size_ && length <= size_ - offset);
    detail::retain(block_);
    return Buffer(block_, data_ + offset, length);
}

uint32_t Buffer::use_count() const noexcept {
    return block_ ? block_->refs.load(std::memory_order_relaxed) : 0;
}

// ── MutableBuffer ───────────────────────────────────────────────────────────

MutableBuffer::MutableBuffer(MutableBuffer&& other) noexcept
    : block_(std::exchange(other.block_, nullptr)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

MutableBuffer& MutableBuffer::operator=(MutableBuffer&& other) noexcept {
    if (this != &other) {
        detail::release(block_);
        block_ = std::exchange(other.block_, nullptr);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

MutableBuffer::~MutableBuffer() { detail::release(block_); }

MutableBuffer MutableBuffer::allocate(size_t capacity, size_t headroom) {
    return allocate(capacity, headroom, BufferPool::shared());
}

MutableBuffer MutableBuffer::allocate(size_t capacity, size_t headroom, BufferPool& pool) {
    MutableBuffer buffer;
    if (capacity + headroom == 0) return buffer;
    buffer.block_ = pool.acquire(capacity + headroom);
    buffer.data_ = buffer.block_->storage() + headroom;
    return buffer;
}

size_t MutableBuffer::headroom() const noexcept {
    return block_ ? static_cast<size_t>(data_ - block_->storage()) : 0;
}

size_t MutableBuffer::tailroom() const noexcept {
    return block_ ? block_->capacity - headroom() - size_ : 0;
}

uint8_t* MutableBuffer::append(size_t count) noexcept {
    assert(count <= tailroom());
    uint8_t* at = data_ + size_;
    size_ += count;
    return at;
}

void MutableBuffer::append(std::span<const uint8_t> bytes) noexcept {
    if (!bytes.empty()) std::memcpy(append(bytes.size()), bytes.data(), bytes.size());
}

uint8_t* MutableBuffer::prepend(size_t count) noexcept {
    assert(count <= headroom());
    data_ -= count;
    size_ += count;
    return data_;
}

void MutableBuffer::resize(size_t size) noexcept {
    assert(size <= size_ + tailroom());
    size_ = size;
}

Buffer MutableBuffer::freeze() && noexcept {
    Buffer frozen(block_, data_, size_);
    block_ = nullptr;
    data_ = nullptr;
    size_ = 0;
    return frozen;
}

// ── BufferPool ──────────────────────────────────────────────────────────────

BufferPool::BufferPool(Limits limits)
    : limits_(limits),
      class_count_(detail::class_of(limits.max_pooled_block) + 1),
      free_(class_count_, nullptr) {}

BufferPool::~BufferPool() {
    trim();
    assert(stats_.outstanding == 0 && "a BufferPool must outlive its buffers");
}

BufferPool& BufferPool::shared() noexcept {
    // Never destroyed: buffers held by other static objects may be released
    // after main() returns, in any order.
    static BufferPool* const pool = new BufferPool();
    return *pool;
}

size_t BufferPool::block_size(size_t bytes) const noexcept {
    if (bytes > limits_.max_pooled_block) return bytes;
    return detail::class_size(detail::class_of(bytes));
}

BufferPool::Stats BufferPool::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void BufferPool::trim() noexcept {
    std::vector<BufferBlock*> heads;
    {
        std::lock_guard lock(mutex_);
        heads.swap(free_);
        free_.assign(class_count_, nullptr);
        stats_.cached_blocks = 0;
        stats_.cached_bytes = 0;
    }
    for (BufferBlock* block : heads) {
        while (block) detail::delete_block(std::exchange(block, block->next_free));
    }
}

BufferBlock* BufferPool::acquire(size_t bytes) {
    const bool pooled = bytes <= limits_.max_pooled_block;
    const size_t index = pooled ? detail::class_of(bytes) : 0;
    if (pooled) {
        std::lock_guard lock(mutex_);
        if (BufferBlock* block = free_[index]) {
            free_[index] = std::exchange(block->next_free, nullptr);
            stats_.cached_blocks -= 1;
            stats_.cached_bytes -= block->capacity;
            stats_.reuses += 1;
            stats_.outstanding += 1;
            block->refs.store(1, std::memory_order_relaxed);
            return block;
        }
    }

    // Allocated outside the lock: a cache miss on one thread must not stall the
    // hits of every other.
    BufferBlock* block =
        pooled ? detail::new_block(this, detail::class_size(index), static_cast<uint32_t>(index))
               : detail::new_block(this, bytes, BufferBlock::kUnpooled);

    std::lock_guard lock(mutex_);
    stats_.system_allocations += 1;
    stats_.outstanding += 1;
    return block;
}

void BufferPool::release(BufferBlock* block) noexcept {
    {
        std::lock_guard lock(mutex_);
        stats_.outstanding -= 1;
        if (block->size_class != BufferBlock::kUnpooled &&
            stats_.cached_bytes + block->capacity <= limits_.max_cached_bytes) {
            block->next_free = std::exchange(free_[block->size_class], block);
            stats_.cached_blocks += 1;
            stats_.cached_bytes += block->capacity;
            return;
        }
    }
    detail::delete_block(block);
}

}  // namespace ratcast
