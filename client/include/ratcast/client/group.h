#pragma once

/**
 * @file group.h
 * @brief A group: a room that lasts, kept by its members.
 *
 * A group is a key of its own and a secret. Its id (GroupId) is the public
 * key of a key pair its maker keeps — of the kind a person's is (identity.h:
 * PersonKey) — and whatever changes the group is signed by it. Its link
 * (GroupLink: https://ratcast.app/g/#GROUP.SECRET) lets in whoever has it:
 * members meet under keys of the secret, never the id alone — in its rooms,
 * a channel each, each a room like any other (rats_url.h: its RoomSecret of
 * the group's, channel(); the main one, kMainChannel, is its room now), and
 * as their home nodes catch each other up (people.h).
 *
 * Its state is a log (GroupLog): changes, each {seq, type, version, data}
 * signed by the group's key, held by every member and caught up as members
 * meet; the state is what they make in order. A change of a type not known
 * here is kept and passed on, not read: what a later app adds — channels,
 * roles, bans — comes as types of its own, and apps of both kinds share a
 * group. Named (the group's name) is the one type there is now.
 */

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ratcast/client/identity.h"
#include "ratcast/client/rats_url.h"

namespace ratcast {

/// A group: the public key its changes are signed by.
struct GroupId {
    std::array<uint8_t, 32> key{};

    /// The group of `key`, its maker's.
    static GroupId of(const PersonKey& key) { return GroupId{key.id().key}; }
    std::string to_hex() const;
    static std::optional<GroupId> from_hex(std::string_view hex);
    /// Four characters of the key's hash, as a person's (PersonId::tag()).
    std::string tag() const;

    friend auto operator<=>(const GroupId&, const GroupId&) = default;
};

/// A link to a group: its id, and the secret that lets one in.
struct GroupLink {
    GroupId group;
    std::array<uint8_t, 16> secret{};

    friend bool operator==(const GroupLink&, const GroupLink&) = default;
};

/// "rats://g/GROUP.SECRET", or the website's "https://ratcast.app/g/#…";
/// nullopt for anything else.
std::optional<GroupLink> parse_group_link(std::string_view link);
std::string to_web_link(const GroupLink& link);
std::string to_string(const GroupLink& link);

/// The group's room: its channel 0. Another voice channel is another name.
inline constexpr std::string_view kMainChannel = "main";

/// A group's channel, a room: its secret of the group's and the channel's
/// name — never worked out from the id alone. Its members' calls are met
/// there (CallConfig::dial), and who is there watched (room_key()).
RoomSecret channel(const GroupLink& link, std::string_view name = kMainChannel);

/// What a change is, of those known here.
enum class ChangeType : uint16_t {
    Named = 1,  ///< data: the group's name, UTF-8
};

/// A change to a group, signed by its key.
struct GroupChange {
    static constexpr size_t kMostData = 4096;

    uint64_t seq = 0;      ///< its place in the log: 1, 2, …
    uint16_t type = 0;     ///< a ChangeType, or one of a later app's
    uint16_t version = 1;  ///< of its data's form
    std::vector<uint8_t> data;
    std::array<uint8_t, 64> signature{};

    friend bool operator==(const GroupChange&, const GroupChange&) = default;
};

/// The changes made to a group, in order: by seq, two of one seq (made on two
/// of the maker's devices at once) by their signatures — alike everywhere.
class GroupLog {
public:
    /// The most kept; one past it is refused.
    static constexpr size_t kMostChanges = 4096;

    /// A change of `group_key`'s group after the last one, signed: kept here
    /// and returned. Its data past GroupChange::kMostData is cut.
    GroupChange append(const PersonKey& group_key, ChangeType type, std::span<const uint8_t> data,
                       uint16_t version = 1);
    /// `change` kept, if it is `group`'s — signed by its key — and not here
    /// already; whether it was.
    bool add(const GroupId& group, GroupChange change);
    /// Each of `other`'s; whether any was new here.
    bool merge(const GroupId& group, const GroupLog& other);

    const std::vector<GroupChange>& changes() const noexcept { return changes_; }
    bool empty() const noexcept { return changes_.empty(); }
    /// The last change's seq; 0 for none.
    uint64_t last() const noexcept { return changes_.empty() ? 0 : changes_.back().seq; }
    /// The name the last Named change gives; "" for none.
    std::string name() const;

    friend bool operator==(const GroupLog&, const GroupLog&) = default;

private:
    std::vector<GroupChange> changes_;
};

/// The log as bytes, and back: nullopt for bytes that are no log of
/// `group`'s, or with a change it did not sign.
std::vector<uint8_t> encode(const GroupLog& log);
std::optional<GroupLog> decode_group_log(const GroupId& group, std::span<const uint8_t> bytes);

}  // namespace ratcast
