#pragma once

/**
 * @file book.h
 * @brief What a person knows, the same on each of their devices: their name,
 * their links' secrets, their friends, the requests both ways, their groups
 * (people.h).
 *
 * Kept as JSON ("people.json" in the data directory), told whole to the
 * person's other devices as they meet, and merged there (merge()): of each
 * entry, the one changed last wins — a group's log, whatever, is the union
 * of both.
 */

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ratcast/client/group.h"
#include "ratcast/client/identity.h"
#include "ratcast/core/result.h"

namespace ratcast {

using Secret = std::array<uint8_t, 16>;
using Shared = std::array<uint8_t, 32>;

/// Seconds since the Unix epoch.
uint64_t seconds_now();
bool is_zero(std::span<const uint8_t> bytes);
/// A name as it may be told: plain, at most a Presence's.
std::string told_name(std::string name);
/// A group's name as it is kept: told, its ends trimmed.
std::string group_name(std::string name);

/// What a friend request was made with: the person's link, or the card
/// secret a room's member is told (IdentityCard::request_secret).
enum class Via : uint8_t { Link = 0, Card = 1 };

struct FriendEntry {
    std::string name;
    Shared shared{};
    Secret presence{};  ///< theirs, once told; zero before
    uint64_t since = 0;
    uint64_t at = 0;  ///< when last changed: the latest wins
    bool removed = false;
};

struct RequestEntry {
    std::string name;
    bool incoming = false;
    bool declined = false;
    Via via = Via::Link;
    Secret secret{};  ///< an outgoing one's: the secret of theirs it was made with
    uint64_t made = 0;
    uint64_t at = 0;
    bool gone = false;       ///< answered or taken back
    bool delivered = false;  ///< an outgoing one's: told to a device of theirs
};

struct GroupEntry {
    Secret secret{};
    /// The seed of the group's key, if this person made it.
    std::optional<std::array<uint8_t, 32>> key;
    GroupLog log;
    uint64_t since = 0;
    uint64_t at = 0;  ///< when its membership last changed: the latest wins
    bool left = false;

    GroupLink link(const GroupId& group) const { return GroupLink{group, secret}; }
};

struct Book {
    std::string name;
    uint64_t name_at = 0;
    /// The secret of the person's link (People::link()).
    Secret link{};
    uint64_t link_at = 0;
    /// The secret every room's member is told on their card, to ask by.
    Secret card_secret{};
    uint64_t card_secret_at = 0;
    std::map<PersonId, FriendEntry> friends;
    std::map<PersonId, RequestEntry> requests;
    std::map<GroupId, GroupEntry> groups;

    /// A person's first: their name, new secrets.
    static Book fresh(std::string name);
};

std::string to_json(const Book& book, bool pretty = false);
/// The book in `text`; nullopt for anything that is none.
std::optional<Book> book_from(std::string_view text);
/// What `theirs` knows that `mine` does not, taken in; whether anything was.
bool merge(Book& mine, const Book& theirs);

/// The book kept at `path`, or nullopt where there is none (or it is broken).
std::optional<Book> load_book(const std::filesystem::path& path);
/// Kept at `path`, written whole to a file beside it first and moved over.
Result<void> save_book(const std::filesystem::path& path, const Book& book);

}  // namespace ratcast
