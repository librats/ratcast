#include "session/sframe.h"

#include <cassert>
#include <cstring>
#include <random>
#include <string_view>
#include <vector>

#include "ratcast/stream/frame.h"

#include "librats/crypto/chachapoly.h"
#include "librats/crypto/hkdf.h"

namespace ratcast::sframe {
namespace {

constexpr uint8_t kExtendedKid = 0x80;  // X
constexpr unsigned kKidShift = 4;
constexpr uint8_t kExtendedCtr = 0x08;  // Y
constexpr uint8_t kValueBits = 0x07;
constexpr uint64_t kShortLimit = 8;  // a value below fits in the config byte

static_assert(kKeySize == RATS_CHACHAPOLY_KEY_SIZE && kTagSize == RATS_CHACHAPOLY_TAG_SIZE);

/// Bytes of `value`, big-endian, the fewest it fits in: 1 to 8.
size_t length_of(uint64_t value) noexcept {
    size_t n = 1;
    while (n < 8 && value >> (8 * n) != 0) ++n;
    return n;
}

void put_be(uint64_t value, size_t n, uint8_t* out) noexcept {
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(value >> (8 * (n - 1 - i)));
}

/// HKDF-Expand(secret, label | KID | suite, n), RFC 9605 §4.4.2.
void expand(const uint8_t* secret, std::string_view label, uint64_t kid, uint8_t* out, size_t n) {
    std::vector<uint8_t> info(label.begin(), label.end());
    info.resize(label.size() + 8 + 2);
    put_be(kid, 8, info.data() + label.size());
    put_be(kCipherSuite, 2, info.data() + label.size() + 8);
    rats_hkdf_sha256_expand(secret, info.data(), info.size(), out, n);
}

/// The SFrame header, then the metadata — a frame's header, which fits in
/// the headroom left for it: what the AEAD authenticates.
class Aad {
public:
    Aad(std::span<const uint8_t> header, std::span<const uint8_t> metadata) noexcept
        : size_(header.size() + metadata.size()) {
        if (!fits()) return;
        std::memcpy(bytes_.data(), header.data(), header.size());
        std::memcpy(bytes_.data() + header.size(), metadata.data(), metadata.size());
    }
    bool fits() const noexcept { return size_ <= bytes_.size(); }
    const uint8_t* data() const noexcept { return bytes_.data(); }
    size_t size() const noexcept { return size_; }

private:
    std::array<uint8_t, kMaxHeaderSize + kFrameHeadroom> bytes_;
    size_t size_;
};

}  // namespace

BaseKey random_key() {
    // The platform's entropy source, as librats takes its Noise keys.
    std::random_device device;
    BaseKey key;
    for (uint8_t& byte : key) byte = static_cast<uint8_t>(device());
    return key;
}

size_t header_size(const Header& header) noexcept {
    return 1 + (header.kid < kShortLimit ? 0 : length_of(header.kid)) +
           (header.ctr < kShortLimit ? 0 : length_of(header.ctr));
}

void write_header(const Header& header, uint8_t* out) noexcept {
    uint8_t& config = *out++;
    config = 0;
    if (header.kid < kShortLimit) {
        config |= static_cast<uint8_t>(header.kid << kKidShift);
    } else {
        const size_t n = length_of(header.kid);
        config |= static_cast<uint8_t>(kExtendedKid | (n - 1) << kKidShift);
        put_be(header.kid, n, out);
        out += n;
    }
    if (header.ctr < kShortLimit) {
        config |= static_cast<uint8_t>(header.ctr);
    } else {
        const size_t n = length_of(header.ctr);
        config |= static_cast<uint8_t>(kExtendedCtr | (n - 1));
        put_be(header.ctr, n, out);
    }
}

Result<ReadHeader> read_header(std::span<const uint8_t> sealed) {
    if (sealed.empty()) return fail(Errc::Truncated);
    const uint8_t config = sealed[0];
    size_t at = 1;
    const auto value = [&](bool extended, uint8_t bits) -> std::optional<uint64_t> {
        if (!extended) return bits;
        const size_t n = size_t{bits} + 1;
        if (sealed.size() < at + n) return std::nullopt;
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v = v << 8 | sealed[at + i];
        at += n;
        return v;
    };
    const std::optional<uint64_t> kid =
        value(config & kExtendedKid, static_cast<uint8_t>(config >> kKidShift & kValueBits));
    const std::optional<uint64_t> ctr = value(config & kExtendedCtr, config & kValueBits);
    if (!kid || !ctr) return fail(Errc::Truncated);
    return ReadHeader{{*kid, *ctr}, at};
}

Key::Key(uint64_t kid, const BaseKey& base) : kid_(kid) {
    uint8_t secret[RATS_HKDF_SHA256_HASH_LEN];
    rats_hkdf_sha256_extract(nullptr, 0, base.data(), base.size(), secret);
    expand(secret, "SFrame 1.0 Secret key ", kid, key_.data(), key_.size());
    expand(secret, "SFrame 1.0 Secret salt ", kid, salt_.data(), salt_.size());
    std::memset(secret, 0, sizeof secret);
}

std::array<uint8_t, 12> Key::nonce(uint64_t ctr) const noexcept {
    std::array<uint8_t, 12> nonce = salt_;
    for (size_t i = 0; i < 8; ++i) nonce[4 + i] ^= static_cast<uint8_t>(ctr >> (8 * (7 - i)));
    return nonce;
}

MutableBuffer Key::seal(uint64_t ctr, std::span<const uint8_t> metadata,
                        std::span<const uint8_t> plaintext) const {
    const Header header{kid_, ctr};
    const size_t size = header_size(header);
    MutableBuffer sealed = allocate_frame_payload(size + plaintext.size() + kTagSize);
    uint8_t* out = sealed.append(size + plaintext.size() + kTagSize);
    write_header(header, out);
    const Aad aad(std::span(out, size), metadata);
    assert(aad.fits());
    const std::array<uint8_t, 12> iv = nonce(ctr);
    rats_chachapoly_encrypt(key_.data(), iv.data(), aad.data(), aad.size(), plaintext.data(),
                            plaintext.size(), out + size);
    return sealed;
}

Result<Buffer> Key::open(const ReadHeader& header, std::span<const uint8_t> metadata,
                         std::span<const uint8_t> sealed) const {
    if (header.header.kid != kid_ || sealed.size() < header.size + kTagSize) {
        return fail(Errc::Malformed);
    }
    const std::span<const uint8_t> ciphertext = sealed.subspan(header.size);
    MutableBuffer plain = MutableBuffer::allocate(ciphertext.size() - kTagSize, 0);
    uint8_t* out = plain.append(ciphertext.size() - kTagSize);
    const Aad aad(sealed.first(header.size), metadata);
    const std::array<uint8_t, 12> iv = nonce(header.header.ctr);
    if (!aad.fits() ||
        rats_chachapoly_decrypt(key_.data(), iv.data(), aad.data(), aad.size(), ciphertext.data(),
                                ciphertext.size(), out) == RATS_CHACHAPOLY_DECRYPT_FAILED) {
        return fail(Errc::Malformed);
    }
    return std::move(plain).freeze();
}

}  // namespace ratcast::sframe
