#include "ratcast/client/group.h"

#include <algorithm>
#include <tuple>

#include "bytes.h"
#include "key_link.h"

namespace ratcast {
namespace {

constexpr std::string_view kKind = "g";
/// The form a log is written in, and a reset's data.
constexpr uint8_t kLogForm = 1;
constexpr uint8_t kResetForm = 1;

/// What a change's signature is of: the group's, and each of its parts.
std::vector<uint8_t> signed_part(const GroupId& group, const GroupChange& change) {
    ByteWriter out;
    out.bytes(bytes_of("ratcast group change"));
    out.bytes(group.key);
    out.u64(change.seq);
    out.u16(change.type);
    out.u16(change.version);
    out.u32(change.data.size());
    out.bytes(change.data);
    return out.take();
}

bool before(const GroupChange& a, const GroupChange& b) {
    return std::tie(a.seq, a.signature) < std::tie(b.seq, b.signature);
}

}  // namespace

std::string GroupId::to_hex() const { return ratcast::to_hex(key); }

std::optional<GroupId> GroupId::from_hex(std::string_view hex) {
    const std::optional<PersonId> id = PersonId::from_hex(hex);
    if (!id) return std::nullopt;
    return GroupId{id->key};
}

std::string GroupId::tag() const { return PersonId{key}.tag(); }

std::optional<GroupLink> parse_group_link(std::string_view link) {
    const std::optional<key_link::KeyAndSecret> parsed = key_link::parse(link, kKind);
    if (!parsed) return std::nullopt;
    return GroupLink{.group = GroupId{parsed->key}, .secret = parsed->secret};
}

std::string to_web_link(const GroupLink& link) {
    return key_link::web(kKind, link.group.key, link.secret);
}

std::string to_string(const GroupLink& link) {
    return key_link::rats(kKind, link.group.key, link.secret);
}

RoomSecret channel(const GroupLink& link, std::string_view name) {
    ByteWriter message;
    message.bytes(bytes_of("ratcast channel"));
    message.bytes(link.group.key);
    message.bytes(bytes_of(name));
    const std::array<uint8_t, 32> full = mac(link.secret, message.bytes());
    RoomSecret room;
    std::copy_n(full.begin(), room.bytes.size(), room.bytes.begin());
    return room;
}

std::array<uint8_t, 32> secret_mark(const GroupLink& link) {
    ByteWriter message;
    message.bytes(bytes_of("ratcast group secret"));
    message.bytes(link.group.key);
    return mac(link.secret, message.bytes());
}

// A reset's data: u8 form, the 32-byte mark, u16 count, the kept persons'
// 32-byte keys.
std::vector<uint8_t> encode(const GroupReset& reset) {
    ByteWriter out;
    out.u8(kResetForm);
    out.bytes(reset.mark);
    const size_t count = std::min(reset.kept.size(), GroupReset::kMostKept);
    out.u16(count);
    for (size_t i = 0; i < count; ++i) out.bytes(reset.kept[i].key);
    return out.take();
}

std::optional<GroupReset> decode_group_reset(std::span<const uint8_t> data) {
    ByteReader in(data);
    if (in.u8() != kResetForm) return std::nullopt;
    GroupReset reset{.mark = in.array<32>()};
    const size_t count = in.u16();
    if (!in.ok() || count > GroupReset::kMostKept) return std::nullopt;
    for (size_t i = 0; i < count; ++i) reset.kept.push_back(PersonId{in.array<32>()});
    if (!in.ok() || !in.done()) return std::nullopt;
    return reset;
}

// ── The log ─────────────────────────────────────────────────────────────────

GroupChange GroupLog::append(const PersonKey& group_key, ChangeType type,
                             std::span<const uint8_t> data, uint16_t version) {
    const GroupId group = GroupId::of(group_key);
    GroupChange change{.seq = last() + 1, .type = uint16_t(type), .version = version};
    change.data.assign(data.begin(), data.begin() + static_cast<ptrdiff_t>(std::min(
                                                        data.size(), GroupChange::kMostData)));
    change.signature = group_key.sign(signed_part(group, change));
    add(group, change);
    return change;
}

bool GroupLog::add(const GroupId& group, GroupChange change) {
    if (change.data.size() > GroupChange::kMostData || changes_.size() >= kMostChanges)
        return false;
    const auto at = std::lower_bound(changes_.begin(), changes_.end(), change, before);
    if (at != changes_.end() && at->seq == change.seq && at->signature == change.signature)
        return false;  // here already
    if (!verify(PersonId{group.key}, signed_part(group, change), change.signature)) return false;
    changes_.insert(at, std::move(change));
    return true;
}

bool GroupLog::merge(const GroupId& group, const GroupLog& other) {
    bool added = false;
    for (const GroupChange& change : other.changes_) added = add(group, change) || added;
    return added;
}

std::string GroupLog::name() const {
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) {
        if (it->type == uint16_t(ChangeType::Named))
            return std::string(it->data.begin(), it->data.end());
    }
    return {};
}

std::optional<GroupReset> GroupLog::reset() const {
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) {
        if (it->type != uint16_t(ChangeType::Reset)) continue;
        // One of a later form, or broken: the one before stands.
        if (std::optional<GroupReset> reset = decode_group_reset(it->data)) return reset;
    }
    return std::nullopt;
}

bool GroupLog::current(const GroupLink& link) const {
    const std::optional<GroupReset> last = reset();
    return !last || last->mark == secret_mark(link);
}

std::vector<uint8_t> encode(const GroupLog& log) {
    ByteWriter out;
    out.u8(kLogForm);
    out.u32(log.changes().size());
    for (const GroupChange& change : log.changes()) {
        out.u64(change.seq);
        out.u16(change.type);
        out.u16(change.version);
        out.u32(change.data.size());
        out.bytes(change.data);
        out.bytes(change.signature);
    }
    return out.take();
}

std::optional<GroupLog> decode_group_log(const GroupId& group, std::span<const uint8_t> bytes) {
    ByteReader in(bytes);
    if (in.u8() != kLogForm) return std::nullopt;
    const size_t count = in.u32();
    if (!in.ok() || count > GroupLog::kMostChanges) return std::nullopt;
    GroupLog log;
    for (size_t i = 0; i < count; ++i) {
        GroupChange change;
        change.seq = in.u64();
        change.type = uint16_t(in.u16());
        change.version = uint16_t(in.u16());
        const size_t size = in.u32();
        if (!in.ok() || size > GroupChange::kMostData) return std::nullopt;
        const std::span<const uint8_t> data = in.bytes(size);
        change.data.assign(data.begin(), data.end());
        change.signature = in.array<64>();
        if (!in.ok()) return std::nullopt;
        // Unsigned is no log; twice is no harm.
        if (!verify(PersonId{group.key}, signed_part(group, change), change.signature))
            return std::nullopt;
        log.add(group, std::move(change));
    }
    if (!in.done()) return std::nullopt;
    return log;
}

}  // namespace ratcast
