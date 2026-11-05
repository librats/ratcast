#pragma once

/**
 * @file bytes.h
 * @brief What the client's own messages are written in and read from: big
 * endian numbers, bytes, texts with their length before them.
 *
 * A reader that runs past the end reads zeros from then on and says so
 * (ok()): what was read is looked at once, at the end.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ratcast {

class ByteWriter {
public:
    void u8(uint8_t value) { bytes_.push_back(value); }
    void u16(size_t value) { be(value, 2); }
    void u32(size_t value) { be(value, 4); }
    void u64(uint64_t value) { be(value, 8); }
    void bytes(std::span<const uint8_t> bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }
    /// A text of 255 bytes at most, its length a byte before it.
    void text8(std::string_view text) {
        const size_t size = std::min<size_t>(text.size(), 255);
        u8(uint8_t(size));
        bytes_.insert(bytes_.end(), text.begin(), text.begin() + static_cast<ptrdiff_t>(size));
    }
    void text32(std::string_view text) {
        u32(text.size());
        bytes_.insert(bytes_.end(), text.begin(), text.end());
    }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    std::vector<uint8_t> take() { return std::move(bytes_); }

private:
    void be(uint64_t value, int size) {
        for (int shift = (size - 1) * 8; shift >= 0; shift -= 8)
            bytes_.push_back(uint8_t(value >> shift));
    }

    std::vector<uint8_t> bytes_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    bool ok() const { return ok_; }
    /// Everything read, and nothing more there.
    bool done() const { return ok_ && at_ == bytes_.size(); }
    uint8_t u8() {
        if (!need(1)) return 0;
        return bytes_[at_++];
    }
    size_t u16() { return size_t(be(2)); }
    size_t u32() { return size_t(be(4)); }
    uint64_t u64() { return be(8); }
    std::span<const uint8_t> bytes(size_t count) {
        if (!need(count)) return {};
        const std::span<const uint8_t> view = bytes_.subspan(at_, count);
        at_ += count;
        return view;
    }
    template <size_t N>
    std::array<uint8_t, N> array() {
        std::array<uint8_t, N> out{};
        const std::span<const uint8_t> view = bytes(N);
        if (ok_) std::copy(view.begin(), view.end(), out.begin());
        return out;
    }
    std::string text(size_t size) {
        const std::span<const uint8_t> view = bytes(size);
        return std::string(view.begin(), view.end());
    }

private:
    uint64_t be(int size) {
        uint64_t value = 0;
        for (int i = 0; i < size; ++i) value = (value << 8) | u8();
        return value;
    }
    bool need(size_t count) {
        if (!ok_ || bytes_.size() - at_ < count) ok_ = false;
        return ok_;
    }

    std::span<const uint8_t> bytes_;
    size_t at_ = 0;
    bool ok_ = true;
};

/// A text's bytes, as they are.
inline std::span<const uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

}  // namespace ratcast
