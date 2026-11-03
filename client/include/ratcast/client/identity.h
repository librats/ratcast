#pragma once

/**
 * @file identity.h
 * @brief A person, whatever device they are on: a key of their own.
 *
 * A person is the public key of a key pair kept on each of their devices
 * (PersonKey; EdDSA, Monocypher's: Ed25519 over BLAKE2b). Each librats node
 * a device runs — a room's maker, a guest, the home node of people.h — is the
 * person's by a certificate the key signs (Certificate: the node's peer id,
 * when it was made), told to whoever it meets (an IdentityCard in its
 * Presence). Noise already proves the peer id; the certificate proves whose
 * it is.
 *
 * Others know the person by the key, and tell two of one name apart by its
 * tag (PersonId::tag(): four characters of its hash, "4F2K").
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/session/media_network.h"

namespace ratcast {

/// Bytes from the system's source of randomness.
void random_bytes(std::span<uint8_t> out);

template <size_t N>
std::array<uint8_t, N> random_bytes() {
    std::array<uint8_t, N> bytes{};
    random_bytes(bytes);
    return bytes;
}

/// A MAC of `message` under `key` (keyed BLAKE2b, 32 bytes; a key of 1 to 64
/// bytes).
std::array<uint8_t, 32> mac(std::span<const uint8_t> key, std::span<const uint8_t> message);

/// Whether two MACs, or any two secrets of a size, are alike, in a time that
/// says nothing of where they differ.
bool same_secret(std::span<const uint8_t> a, std::span<const uint8_t> b) noexcept;

/// The bytes in hex, and back (nullopt for anything that is not hex of
/// `size` bytes).
std::string to_hex(std::span<const uint8_t> bytes);
std::optional<std::vector<uint8_t>> from_hex(std::string_view hex, size_t size);

/// The bytes in base64url without padding, and back (nullopt for anything
/// that is not `size` bytes so written).
std::string to_base64url(std::span<const uint8_t> bytes);
std::optional<std::vector<uint8_t>> from_base64url(std::string_view text, size_t size);

/// A person: the public key their devices are certified by.
struct PersonId {
    std::array<uint8_t, 32> key{};

    std::string to_hex() const;
    static std::optional<PersonId> from_hex(std::string_view hex);
    /// Four characters of the key's hash (Crockford's base32: no I, L, O or
    /// U), what tells two of one name apart.
    std::string tag() const;

    friend auto operator<=>(const PersonId&, const PersonId&) = default;
};

/// A person's key pair, on each of their devices: what certifies them.
class PersonKey {
public:
    /// A new one, from the system's randomness.
    static PersonKey generate();
    /// The one of `seed`, as seed() gives it.
    static PersonKey from_seed(std::span<const uint8_t, 32> seed);
    /// The one kept in `path` (written by save()); Unavailable where there is
    /// none, Malformed where it is something else.
    static Result<PersonKey> load(const std::filesystem::path& path);
    /// The one kept in `path`, or a new one kept there.
    static Result<PersonKey> load_or_create(const std::filesystem::path& path);

    PersonKey(const PersonKey&) = default;
    PersonKey& operator=(const PersonKey&) = default;
    ~PersonKey();

    /// Kept in `path` as text — what a backup is too — its folder made.
    Result<void> save(const std::filesystem::path& path) const;
    /// As save() writes it, and back.
    std::string to_text() const;
    static std::optional<PersonKey> from_text(std::string_view text);

    const PersonId& id() const noexcept { return id_; }
    /// The 32 bytes the key pair is made of: the secret, to hand to another
    /// device of the person's and nobody else.
    std::span<const uint8_t, 32> seed() const noexcept { return seed_; }
    std::array<uint8_t, 64> sign(std::span<const uint8_t> message) const;

private:
    PersonKey() = default;

    std::array<uint8_t, 32> seed_{};
    std::array<uint8_t, 64> secret_{};  ///< Monocypher's: the seed, then the public key
    PersonId id_;
};

/// Whether `signature` is `person`'s of `message`.
bool verify(const PersonId& person, std::span<const uint8_t> message,
            std::span<const uint8_t, 64> signature);

/// A librats node of a person's: its peer id, signed by the person's key.
struct Certificate {
    PersonId person;
    PeerId device;
    uint64_t issued = 0;  ///< seconds since the Unix epoch
    std::array<uint8_t, 64> signature{};

    friend bool operator==(const Certificate&, const Certificate&) = default;
};

/// `device` certified as `key`'s person's, issued now.
Certificate certify(const PersonKey& key, const PeerId& device);
/// Whether the signature is the person's.
bool is_valid(const Certificate& certificate);

/// Who a peer is, told in its Presence (Presence::identity): its certificate,
/// and the secret anyone who meets it may send a friend request with
/// (people.h; zeros: none taken).
struct IdentityCard {
    static constexpr size_t kSize = 1 + 32 + 32 + 8 + 64 + 16;

    Certificate certificate;
    std::array<uint8_t, 16> request_secret{};

    friend bool operator==(const IdentityCard&, const IdentityCard&) = default;
};

std::vector<uint8_t> encode(const IdentityCard& card);
/// The card in `bytes`, its certificate checked; nullopt for anything else.
/// A later version may append to it.
std::optional<IdentityCard> decode_identity(std::span<const uint8_t> bytes);
/// Whose `peer` is, by the card its presence carries: the card if its
/// certificate is valid and of `peer`.
std::optional<IdentityCard> identity_of(std::span<const uint8_t> presence_identity,
                                        const PeerId& peer);

}  // namespace ratcast
