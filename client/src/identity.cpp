#include "ratcast/client/identity.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

#include "monocypher.h"

namespace ratcast {
namespace {

constexpr uint8_t kCardVersion = 1;
constexpr std::string_view kKeyHeader = "ratcast person key";
/// What a certificate signs: this, then the person, the device, when.
constexpr std::string_view kCertifies = "ratcast certificate 1";

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr char kBase64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
/// Crockford's base32.
constexpr char kTagDigits[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int base64url_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

void put_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(uint8_t(value >> shift));
}

uint64_t get_u64(std::span<const uint8_t> in) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = (value << 8) | in[i];
    return value;
}

/// What a certificate's signature is of.
std::vector<uint8_t> certified(const PersonId& person, const PeerId& device, uint64_t issued) {
    std::vector<uint8_t> message(kCertifies.begin(), kCertifies.end());
    message.insert(message.end(), person.key.begin(), person.key.end());
    message.insert(message.end(), device.bytes().begin(), device.bytes().end());
    put_u64(message, issued);
    return message;
}

uint64_t seconds_now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
}

}  // namespace

// ── Bytes ───────────────────────────────────────────────────────────────────

void random_bytes(std::span<uint8_t> out) {
    // The system's CSPRNG behind it on every platform ratcast builds for
    // (as librats' keys are made: crypto/noise.cpp).
    std::random_device device;
    for (size_t i = 0; i < out.size(); i += 4) {
        const uint32_t word = device();
        for (size_t b = 0; b < 4 && i + b < out.size(); ++b) out[i + b] = uint8_t(word >> (8 * b));
    }
}

std::array<uint8_t, 32> mac(std::span<const uint8_t> key, std::span<const uint8_t> message) {
    std::array<uint8_t, 32> out{};
    crypto_blake2b_keyed(out.data(), out.size(), key.data(), std::min<size_t>(key.size(), 64),
                         message.data(), message.size());
    return out;
}

bool same_secret(std::span<const uint8_t> a, std::span<const uint8_t> b) noexcept {
    if (a.size() != b.size()) return false;
    uint8_t differ = 0;
    for (size_t i = 0; i < a.size(); ++i) differ |= uint8_t(a[i] ^ b[i]);
    return differ == 0;
}

std::string to_hex(std::span<const uint8_t> bytes) {
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const uint8_t b : bytes) {
        text += kHexDigits[b >> 4];
        text += kHexDigits[b & 15];
    }
    return text;
}

std::optional<std::vector<uint8_t>> from_hex(std::string_view hex, size_t size) {
    if (hex.size() != size * 2) return std::nullopt;
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        const int high = hex_value(hex[2 * i]);
        const int low = hex_value(hex[2 * i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        bytes[i] = uint8_t(high * 16 + low);
    }
    return bytes;
}

std::string to_base64url(std::span<const uint8_t> bytes) {
    std::string text;
    uint32_t bits = 0;
    int count = 0;
    for (const uint8_t b : bytes) {
        bits = (bits << 8) | b;
        count += 8;
        while (count >= 6) {
            count -= 6;
            text += kBase64url[(bits >> count) & 63];
        }
    }
    if (count > 0) text += kBase64url[(bits << (6 - count)) & 63];
    return text;
}

std::optional<std::vector<uint8_t>> from_base64url(std::string_view text, size_t size) {
    if (text.size() != (size * 8 + 5) / 6) return std::nullopt;
    std::vector<uint8_t> bytes;
    bytes.reserve(size);
    uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        const int value = base64url_value(c);
        if (value < 0) return std::nullopt;
        bits = (bits << 6) | uint32_t(value);
        count += 6;
        if (count >= 8) {
            count -= 8;
            bytes.push_back(uint8_t(bits >> count));
        }
    }
    // The bits past the last byte are zeros, as to_base64url() writes them.
    if (bytes.size() != size || (bits & ((1u << count) - 1)) != 0) return std::nullopt;
    return bytes;
}

// ── A person ────────────────────────────────────────────────────────────────

std::string PersonId::to_hex() const { return ratcast::to_hex(key); }

std::optional<PersonId> PersonId::from_hex(std::string_view hex) {
    const std::optional<std::vector<uint8_t>> bytes = ratcast::from_hex(hex, 32);
    if (!bytes) return std::nullopt;
    PersonId id;
    std::copy(bytes->begin(), bytes->end(), id.key.begin());
    return id;
}

std::string PersonId::tag() const {
    std::array<uint8_t, 32> hash{};
    crypto_blake2b(hash.data(), hash.size(), key.data(), key.size());
    const uint32_t bits = (uint32_t(hash[0]) << 12) | (uint32_t(hash[1]) << 4) | (hash[2] >> 4);
    std::string tag;
    for (int shift = 15; shift >= 0; shift -= 5) tag += kTagDigits[(bits >> shift) & 31];
    return tag;
}

PersonKey PersonKey::generate() {
    std::array<uint8_t, 32> seed = random_bytes<32>();
    PersonKey key = from_seed(seed);
    crypto_wipe(seed.data(), seed.size());
    return key;
}

PersonKey PersonKey::from_seed(std::span<const uint8_t, 32> seed) {
    PersonKey key;
    std::copy(seed.begin(), seed.end(), key.seed_.begin());
    // Monocypher wipes the seed it is given: a copy.
    std::array<uint8_t, 32> wiped = key.seed_;
    crypto_eddsa_key_pair(key.secret_.data(), key.id_.key.data(), wiped.data());
    return key;
}

PersonKey::~PersonKey() {
    crypto_wipe(seed_.data(), seed_.size());
    crypto_wipe(secret_.data(), secret_.size());
}

std::string PersonKey::to_text() const {
    return std::string(kKeyHeader) + "\n" + ratcast::to_hex(seed_) + "\n";
}

std::optional<PersonKey> PersonKey::from_text(std::string_view text) {
    std::string_view rest = text;
    const auto line = [&rest]() {
        const size_t end = rest.find('\n');
        std::string_view got = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
        while (!got.empty() && (got.back() == '\r' || got.back() == ' ')) got.remove_suffix(1);
        return got;
    };
    if (line() != kKeyHeader) return std::nullopt;
    std::optional<std::vector<uint8_t>> seed = ratcast::from_hex(line(), 32);
    if (!seed) return std::nullopt;
    std::array<uint8_t, 32> bytes{};
    std::copy(seed->begin(), seed->end(), bytes.begin());
    crypto_wipe(seed->data(), seed->size());
    PersonKey key = from_seed(bytes);
    crypto_wipe(bytes.data(), bytes.size());
    return key;
}

Result<PersonKey> PersonKey::load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail(Errc::Unavailable);
    std::ostringstream text;
    text << file.rdbuf();
    std::optional<PersonKey> key = from_text(text.str());
    if (!key) return fail(Errc::Malformed);
    return std::move(*key);
}

Result<PersonKey> PersonKey::load_or_create(const std::filesystem::path& path) {
    Result<PersonKey> kept = load(path);
    if (kept || kept.error() != Errc::Unavailable) return kept;
    PersonKey key = generate();
    if (Result<void> saved = key.save(path); !saved) return fail(saved.error());
    return key;
}

Result<void> PersonKey::save(const std::filesystem::path& path) const {
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    // Written aside, then moved in: a key half written is never the one kept.
    std::filesystem::path written = path;
    written += ".new";
    {
        std::ofstream file(written, std::ios::binary | std::ios::trunc);
        if (!file) return fail(Errc::Unavailable);
        file << to_text();
        if (!file.flush()) return fail(Errc::Unavailable);
    }
    std::filesystem::rename(written, path, error);
    if (error) return fail(Errc::Unavailable);
    return {};
}

std::array<uint8_t, 64> PersonKey::sign(std::span<const uint8_t> message) const {
    std::array<uint8_t, 64> signature{};
    crypto_eddsa_sign(signature.data(), secret_.data(), message.data(), message.size());
    return signature;
}

bool verify(const PersonId& person, std::span<const uint8_t> message,
            std::span<const uint8_t, 64> signature) {
    return crypto_eddsa_check(signature.data(), person.key.data(), message.data(),
                              message.size()) == 0;
}

// ── Certificates ────────────────────────────────────────────────────────────

Certificate certify(const PersonKey& key, const PeerId& device) {
    Certificate certificate{.person = key.id(), .device = device, .issued = seconds_now()};
    certificate.signature =
        key.sign(certified(certificate.person, certificate.device, certificate.issued));
    return certificate;
}

bool is_valid(const Certificate& certificate) {
    return verify(certificate.person,
                  certified(certificate.person, certificate.device, certificate.issued),
                  certificate.signature);
}

std::vector<uint8_t> encode(const IdentityCard& card) {
    std::vector<uint8_t> out;
    out.reserve(IdentityCard::kSize);
    out.push_back(kCardVersion);
    const Certificate& c = card.certificate;
    out.insert(out.end(), c.person.key.begin(), c.person.key.end());
    out.insert(out.end(), c.device.bytes().begin(), c.device.bytes().end());
    put_u64(out, c.issued);
    out.insert(out.end(), c.signature.begin(), c.signature.end());
    out.insert(out.end(), card.request_secret.begin(), card.request_secret.end());
    return out;
}

std::optional<IdentityCard> decode_identity(std::span<const uint8_t> bytes) {
    if (bytes.size() < IdentityCard::kSize || bytes[0] != kCardVersion) return std::nullopt;
    IdentityCard card;
    Certificate& c = card.certificate;
    size_t at = 1;
    std::copy_n(bytes.begin() + at, 32, c.person.key.begin());
    at += 32;
    const std::optional<PeerId> device =
        PeerId::from_bytes(librats::ByteView(bytes.data() + at, 32));
    if (!device) return std::nullopt;
    c.device = *device;
    at += 32;
    c.issued = get_u64(bytes.subspan(at, 8));
    at += 8;
    std::copy_n(bytes.begin() + at, 64, c.signature.begin());
    at += 64;
    std::copy_n(bytes.begin() + at, 16, card.request_secret.begin());
    if (!is_valid(c)) return std::nullopt;
    return card;
}

std::optional<IdentityCard> identity_of(std::span<const uint8_t> presence_identity,
                                        const PeerId& peer) {
    std::optional<IdentityCard> card = decode_identity(presence_identity);
    if (!card || card->certificate.device != peer) return std::nullopt;
    return card;
}

}  // namespace ratcast
