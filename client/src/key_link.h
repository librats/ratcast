#pragma once

/**
 * @file key_link.h
 * @brief Links that carry a key and a secret: a person's (people.h: "u" a
 * friend's, "d" a device's) and a group's (group.h: "g").
 *
 * Each as the website's page, "https://ratcast.app/KIND/#KEY.SECRET" — what
 * follows '#' never reaches its server — or as "rats://KIND/KEY.SECRET"; the
 * key (32 bytes) and the secret (16) in base64url.
 */

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ratcast::key_link {

struct KeyAndSecret {
    std::array<uint8_t, 32> key{};
    std::array<uint8_t, 16> secret{};
};

/// What `link` carries, if it is one of `kind`: the website's (with its
/// www., over http too, its '.' escaped by a messenger as well), or rats://.
std::optional<KeyAndSecret> parse(std::string_view link, std::string_view kind);

/// The link of `kind`, the website's or the rats:// one.
std::string web(std::string_view kind, std::span<const uint8_t, 32> key,
                std::span<const uint8_t, 16> secret);
std::string rats(std::string_view kind, std::span<const uint8_t, 32> key,
                 std::span<const uint8_t, 16> secret);

}  // namespace ratcast::key_link
