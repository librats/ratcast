#pragma once

/**
 * @file sframe.h
 * @brief Frames sealed end to end: SFrame (RFC 9605) over ChaCha20-Poly1305.
 *
 * A sender seals each frame's payload under its own key, which it gives each
 * viewer itself (wire::FrameKey, on the connection between them: Noise, end
 * to end even where a server carries it). Those passing frames on — viewers
 * that relay, a server that forwards — have the frame's header to go by and
 * the payload only as ciphertext, which they cannot read nor alter unseen:
 * the header is authenticated with it.
 *
 * SFrame's format and key schedule, as RFC 9605 has them:
 *
 *   sealed payload  SFrame header, ciphertext, 16-byte tag
 *   SFrame header   u8 config |X|K K K|Y|C C C|, then the KID's bytes if X,
 *                   then the CTR's if Y — each big-endian, the fewest bytes
 *                   it fits in (K, C: their count less one); a value under 8
 *                   is the config's bits itself (X or Y 0)
 *   secret          HKDF-Extract("", base key)
 *   key, salt       HKDF-Expand(secret, "SFrame 1.0 Secret key " | KID | suite,
 *                   32), the same with "…salt…", 12 — KID u64, suite u16
 *   nonce           salt XOR CTR, CTR as the last 8 of 12 bytes
 *   AAD             the SFrame header, then the metadata: the frame's header
 *                   as the wire carries it
 *
 * with an AEAD RFC 9605 does not list — ChaCha20-Poly1305, librats' own, under
 * a suite of the range it leaves to private use (kCipherSuite) — so that no
 * other cryptographic library is needed. Interworking with another SFrame is
 * not the aim; a well-reviewed format and key schedule are.
 *
 * A CTR is never used twice under one key: the sender counts its frames, of
 * every stream, and takes a new key long before the count could run out.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "ratcast/core/buffer.h"
#include "ratcast/core/result.h"

namespace ratcast::sframe {

/// The suite, in RFC 9605's range for private use: ChaCha20-Poly1305, its
/// keys from HKDF-SHA256.
inline constexpr uint16_t kCipherSuite = 0xF0CC;
inline constexpr size_t kKeySize = 32;
inline constexpr size_t kTagSize = 16;
/// The longest SFrame header: the config byte, an 8-byte KID and CTR.
inline constexpr size_t kMaxHeaderSize = 1 + 8 + 8;
/// The most sealing adds to a payload.
inline constexpr size_t kMaxOverhead = kMaxHeaderSize + kTagSize;

using BaseKey = std::array<uint8_t, kKeySize>;

/// A fresh random base key.
BaseKey random_key();

struct Header {
    uint64_t kid = 0;  ///< which key
    uint64_t ctr = 0;  ///< which frame under it
    friend bool operator==(const Header&, const Header&) = default;
};

size_t header_size(const Header& header) noexcept;
/// Writes header_size() bytes at `out`.
void write_header(const Header& header, uint8_t* out) noexcept;

struct ReadHeader {
    Header header;
    size_t size = 0;
};
/// The SFrame header `sealed` starts with. Fails with Truncated where it is
/// cut short.
Result<ReadHeader> read_header(std::span<const uint8_t> sealed);

/// One key of a sender's: what its base key and KID give.
class Key {
public:
    Key(uint64_t kid, const BaseKey& base);

    uint64_t kid() const noexcept { return kid_; }

    /// `plaintext` sealed as frame `ctr` under this key, behind `metadata`
    /// (authenticated, not carried): a payload for the wire, with room in
    /// front for its header (allocate_frame_payload()).
    MutableBuffer seal(uint64_t ctr, std::span<const uint8_t> metadata,
                       std::span<const uint8_t> plaintext) const;

    /// The plaintext of `sealed`, whose header `header` (read_header()) names
    /// this key, behind `metadata`. Fails with Malformed where it is not one
    /// this key sealed behind that metadata.
    Result<Buffer> open(const ReadHeader& header, std::span<const uint8_t> metadata,
                        std::span<const uint8_t> sealed) const;

private:
    std::array<uint8_t, 12> nonce(uint64_t ctr) const noexcept;

    uint64_t kid_;
    std::array<uint8_t, kKeySize> key_;
    std::array<uint8_t, 12> salt_;
};

}  // namespace ratcast::sframe
