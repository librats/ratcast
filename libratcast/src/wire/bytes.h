#pragma once

/**
 * @file bytes.h
 * @brief Writing and reading the primitives of libratcast's wire format.
 *
 * Integers are unsigned LEB128 varints: 7 bits per byte, low bits first, the
 * top bit set on every byte but the last. Small numbers — stream ids, layer
 * counts, reference deltas, frame numbers early in a session — take a byte or
 * two instead of a fixed four or eight.
 *
 * Only the minimal encoding of a value is valid. A decoder that accepted the
 * others would let one value travel as many byte strings, which buys nothing
 * and leaves room for every bug that "equal values, different bytes" invites.
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ratcast/result.h"

namespace ratcast::wire {

/// A 64-bit value takes at most ten bytes.
inline constexpr size_t kMaxVarintSize = 10;

constexpr size_t varint_size(uint64_t value) noexcept {
    size_t size = 1;
    for (; value >= 0x80; value >>= 7) ++size;
    return size;
}

/// Writes into space sized beforehand. Header sizes are computed before a
/// header is written, so running out of room is a bug, not an input error.
class Writer {
public:
    explicit Writer(std::span<uint8_t> out) noexcept : out_(out) {}

    void u8(uint8_t value) noexcept {
        assert(position_ < out_.size());
        out_[position_++] = value;
    }

    void varint(uint64_t value) noexcept {
        for (; value >= 0x80; value >>= 7) u8(static_cast<uint8_t>(value | 0x80));
        u8(static_cast<uint8_t>(value));
    }

    size_t position() const noexcept { return position_; }

private:
    std::span<uint8_t> out_;
    size_t position_ = 0;
};

/// Reads bytes front to back, checking every read. The first failure sticks:
/// later reads return 0 and keep the original error. A decoder can therefore
/// read a whole structure and check ok() once — as long as it checks before
/// acting on anything it read.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> in) noexcept : in_(in) {}

    uint8_t u8() noexcept {
        if (failed_) return 0;
        if (position_ == in_.size()) {
            fail(Errc::Truncated);
            return 0;
        }
        return in_[position_++];
    }

    uint64_t varint() noexcept {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            const uint8_t byte = u8();
            if (failed_) return 0;
            value |= uint64_t{byte & 0x7Fu} << shift;
            if ((byte & 0x80) == 0) {
                const bool non_minimal = shift > 0 && byte == 0;
                const bool overflows = shift == 63 && byte > 1;
                if (non_minimal || overflows) {
                    fail(Errc::Malformed);
                    return 0;
                }
                return value;
            }
        }
        fail(Errc::Malformed);  // a continuation bit on the tenth byte
        return 0;
    }

    void skip(size_t count) noexcept {
        if (failed_) return;
        if (count > remaining()) {
            fail(Errc::Truncated);
            return;
        }
        position_ += count;
    }

    /// Record a failure found by the caller's own validation. Only the first
    /// failure is kept.
    void fail(Errc code) noexcept {
        if (failed_) return;
        failed_ = true;
        error_ = code;
    }

    bool ok() const noexcept { return !failed_; }
    Errc error() const noexcept {
        assert(failed_);
        return error_;
    }

    size_t position() const noexcept { return position_; }
    size_t remaining() const noexcept { return in_.size() - position_; }

private:
    std::span<const uint8_t> in_;
    size_t position_ = 0;
    bool failed_ = false;
    Errc error_{};
};

}  // namespace ratcast::wire
