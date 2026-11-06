#include "ratcast/client/people.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <tuple>

#include "book.h"
#include "bytes.h"
#include "key_link.h"
#include "ratcast/client/known_servers.h"

#include "librats/node/node.h"
#include "librats/subsystems/dht_discovery.h"
#include "librats/subsystems/hole_punch.h"
#include "librats/subsystems/port_mapping_service.h"
#include "librats/subsystems/relay.h"
#include "librats/subsystems/rendezvous.h"
#include "librats/util/logger.h"

namespace ratcast {
namespace {

using Steady = std::chrono::steady_clock;
using RingId = std::array<uint8_t, 16>;

constexpr std::string_view kChannel = "ratcast/people";
constexpr std::string_view kKeyFile = "person.key";
constexpr std::string_view kBookFile = "people.json";
constexpr std::string_view kHomeDir = "home";

/// The links' kinds (key_link.h): a friend's, a device's.
constexpr std::string_view kFriendKind = "u";
constexpr std::string_view kDeviceKind = "d";

// ── The wire ────────────────────────────────────────────────────────────────
//
// On the home node's channel, each message: u8 version, u8 kind, then
//   Hello    u16 card length + IdentityCard, u8 name length + name
//   Request  u8 via (0 the person's link, 1 their card's secret), 32-byte proof
//   Answer   u8 accepted, [32-byte shared secret]
//   Pair     32-byte proof, u8 name length + the device's name
//   Grant    32-byte seed, u32 length + the book (JSON)
//   Refuse
//   Sync     u32 length + the book (JSON)
//   Presence 16-byte secret: the sender's person's presence secret, to a friend
//   Ring     16-byte id, u16 length + the room's link: a friend rings; and,
//            a group's room, u8 length + the group's name
//   Rung     16-byte id, u8 accepted: a ring answered — to the caller, and to
//            the answering person's other devices
//   Unring   16-byte id, u8 why (0 hung up, 1 missed, 2 answered elsewhere)
//   Group    32-byte proof, u32 length + the group's log (group.h; none: the
//            sender asks for the receiver's); and u16 count + the members
//            known, a 32-byte person key each (none from a version before)
//   Here     32-byte group id, u8 where (0 not in its room, 1 in it, 2 sharing
//            there): to a member met that proved the group's secret, once it
//            did, and again as it changes
//   Rekey    32-byte proof of the secret the receiver has, 16-byte secret, u32
//            length + the log: the link was reset, the receiver one it keeps
// A proof is the MAC of what it proves under the secret: "ratcast request",
// "ratcast pair", "ratcast group" or "ratcast rekey", the sender's peer id,
// the receiver's — a group's under its secret, the group found by it.

constexpr uint8_t kVersion = 1;
enum class Kind : uint8_t {
    Hello = 1,
    Request,
    Answer,
    Pair,
    Grant,
    Refuse,
    Sync,
    Presence,
    Ring,
    Rung,
    Unring,
    Group,
    Here,
    Rekey
};
enum class Why : uint8_t { HungUp = 0, Missed = 1, AnsweredElsewhere = 2 };
enum class Where : uint8_t { Out = 0, In = 1, Sharing = 2 };

/// The longest room link a ring carries.
constexpr size_t kMostRoomLink = 4096;
/// How much longer a ring rings where it is rung than where it is made: the
/// caller's word that it was missed comes first.
constexpr auto kRingGrace = std::chrono::seconds(5);

constexpr size_t kMostBook = 1 << 20;
/// The most members one Group message tells of.
constexpr size_t kMostMembersTold = 1024;

/// A message: its version and kind, then what the kind says.
class Out : public ByteWriter {
public:
    explicit Out(Kind kind) {
        u8(kVersion);
        u8(uint8_t(kind));
    }
};
using In = ByteReader;

std::array<uint8_t, 32> proof(std::span<const uint8_t> secret, std::string_view what,
                              const PeerId& from, const PeerId& to) {
    std::vector<uint8_t> message(what.begin(), what.end());
    message.insert(message.end(), from.bytes().begin(), from.bytes().end());
    message.insert(message.end(), to.bytes().begin(), to.bytes().end());
    return mac(secret, message);
}

/// The key a person is met under by whoever has `secret` of theirs.
std::string meeting_key(const PersonId& person, std::span<const uint8_t> secret) {
    return "ratcast/0/meet/" + to_hex(mac(secret, person.key));
}

/// The key a person's devices meet each other under: of their key's secret.
std::string devices_key(const PersonKey& key) {
    constexpr std::string_view what = "ratcast devices";
    return "ratcast/0/devices/" + to_hex(mac(key.seed(), bytes_of(what)));
}

/// The secret a person is seen online by, told to friends: of their key's, the
/// same on each of their devices.
Secret presence_secret(const PersonKey& key) {
    constexpr std::string_view what = "ratcast presence";
    const auto full = mac(key.seed(), bytes_of(what));
    Secret out{};
    std::copy_n(full.begin(), out.size(), out.begin());
    return out;
}

/// The key a person's devices are registered under, for friends to watch;
/// and the one they are under while in a room.
std::string presence_key(const PersonId& person, const Secret& secret) {
    return "ratcast/0/online/" + to_hex(mac(secret, person.key));
}
std::string in_room_key(const PersonId& person, const Secret& secret) {
    return "ratcast/0/in-room/" + to_hex(mac(secret, person.key));
}
/// The key a person's devices are rung under: a caller registered there
/// while it rings is introduced to them, and nobody watching is misled.
std::string ring_key(const PersonId& person, const Secret& secret) {
    return "ratcast/0/ring/" + to_hex(mac(secret, person.key));
}

/// The key a group's members' home nodes meet under, to catch each other up:
/// of its secret, as its rooms' are.
std::string group_key(const GroupId& group, const Secret& secret) {
    return "ratcast/0/group/" + to_hex(mac(secret, group.key));
}
/// The key those who have a group's new secret are registered under, at the
/// servers, of the one before its reset: whoever has only that one, watching
/// it, sees that someone is there to meet under it.
std::string reset_key(const GroupId& group, const Secret& secret) {
    ByteWriter message;
    message.bytes(bytes_of("ratcast group reset"));
    message.bytes(group.key);
    return "ratcast/0/group-reset/" + to_hex(mac(secret, message.bytes()));
}

}  // namespace

// ── Links ───────────────────────────────────────────────────────────────────

std::optional<PersonLink> parse_person_link(std::string_view link) {
    for (const auto& [kind, as] : {std::pair{kFriendKind, PersonLink::Kind::Friend},
                                   std::pair{kDeviceKind, PersonLink::Kind::Device}}) {
        if (const std::optional<key_link::KeyAndSecret> parsed = key_link::parse(link, kind))
            return PersonLink{
                .kind = as, .person = PersonId{parsed->key}, .secret = parsed->secret};
    }
    return std::nullopt;
}

namespace {
std::string_view kind_of(const PersonLink& link) {
    return link.kind == PersonLink::Kind::Friend ? kFriendKind : kDeviceKind;
}
}  // namespace

std::string to_web_link(const PersonLink& link) {
    return key_link::web(kind_of(link), link.person.key, link.secret);
}

std::string to_string(const PersonLink& link) {
    return key_link::rats(kind_of(link), link.person.key, link.secret);
}

// ── People ──────────────────────────────────────────────────────────────────

struct People::State {
    /// A device link made here.
    struct Pairing {
        Secret secret{};
        Steady::time_point until{};
        std::optional<PeerId> asking;  ///< the device that proved it, waiting for an answer
        /// The device given the person, until it is met as the person: asked
        /// again (the Grant lost with a link that lost a race), it is given
        /// again; nobody else is.
        std::optional<PeerId> granted;
    };
    /// What is known of a peer met: whose node it is, by its Hello.
    struct Met {
        PersonId person;
        std::string name;
    };

    State(PeopleConfig config_, PeopleHandlers handlers_, PersonKey key_, Book book_)
        : config(std::move(config_)),
          handlers(std::move(handlers_)),
          key(std::move(key_)),
          book(std::move(book_)),
          node(node_config(config)) {}

    static std::string home_dir(const PeopleConfig& config) {
        return config.data_dir.empty()
                   ? std::string()
                   : (std::filesystem::path(config.data_dir) / kHomeDir).string();
    }

    static librats::NodeConfig node_config(const PeopleConfig& config) {
        librats::NodeConfig node;
        node.listen_port = config.port;
        node.bind_address = config.bind_address;
        node.data_dir = home_dir(config);
        node.protocol = config.protocol;
        node.enable_network_monitor = config.watch_network;
        return node;
    }

    std::filesystem::path file(std::string_view name) const {
        return std::filesystem::path(config.data_dir) / name;
    }

    // ── Under the lock ───────────────────────────────────────────────────────

    void save() {
        if (config.data_dir.empty()) return;
        if (!save_book(file(kBookFile), book))
            LOG_WARN("people", "what is known of people not kept");
    }

    void save_key() {
        if (config.data_dir.empty()) return;
        if (!key.save(file(kKeyFile))) LOG_WARN("people", "the person's key not kept");
    }

    /// The keys met under: registered at servers, and in the DHT each both
    /// announced and searched, at both ends — the person's own and those of
    /// others alike. Two behind NATs get through only by dialing each other at
    /// once (each one's dial opening the way for the other's: L21), so each
    /// must find the other, not only the one that asks find the one asked.
    std::vector<std::string> keys() const {
        // The person's own first: a server keeping fewer keeps those.
        std::vector<std::string> keys = {meeting_key(key.id(), book.link),
                                         meeting_key(key.id(), book.card_secret), devices_key(key)};
        if (pairing && Steady::now() < pairing->until)
            keys.push_back(meeting_key(key.id(), pairing->secret));
        if (joining) keys.push_back(meeting_key(joining->person, joining->secret));
        for (const auto& [person, request] : book.requests) {
            if (!request.incoming && !request.declined && !request.gone)
                keys.push_back(meeting_key(person, request.secret));
        }
        for (const auto& [group, entry] : book.groups) {
            if (meets(group, entry)) keys.push_back(group_key(group, entry.secret));
            // Met by those who have only the secret before its reset.
            if (!entry.left && entry.previous) keys.push_back(group_key(group, *entry.previous));
        }
        return keys;
    }

    /// What is registered at servers: those, and the person's presence key —
    /// their room key too while in a room — last: a server that keeps fewer
    /// keys is an older one, which nobody watches at. Not in the DHT: it has
    /// nothing to watch with (L26), and a friend not met there is not seen.
    std::vector<std::string> server_keys() const {
        std::vector<std::string> all = keys();
        for (const auto& [group, entry] : book.groups)
            if (!entry.left && entry.previous) all.push_back(reset_key(group, *entry.previous));
        const Secret own = presence_secret(key);
        all.push_back(presence_key(key.id(), own));
        all.push_back(ring_key(key.id(), own));
        if (in_room) all.push_back(in_room_key(key.id(), own));
        // While ringing a friend: where their devices are.
        if (ringing) {
            const auto it = book.friends.find(ringing->ring.person);
            if (it != book.friends.end() && !is_zero(it->second.presence))
                all.push_back(ring_key(ringing->ring.person, it->second.presence));
        }
        return all;
    }

    /// What is watched at servers: each friend's presence and room keys,
    /// each group's room, and whether its link was reset (reset_key()).
    std::vector<std::string> watched() const {
        std::vector<std::string> out;
        for (const auto& [person, entry] : book.friends) {
            if (entry.removed || is_zero(entry.presence)) continue;
            out.push_back(presence_key(person, entry.presence));
            out.push_back(in_room_key(person, entry.presence));
        }
        for (const auto& [group, entry] : book.groups) {
            if (entry.left) continue;
            out.push_back(room_key(channel(entry.link(group))));
            out.push_back(reset_key(group, entry.secret));
        }
        return out;
    }

    // ── Groups ───────────────────────────────────────────────────────────────

    /// How many are in `group`'s room, as the servers tell.
    size_t present(const GroupId& group, const GroupEntry& entry) const {
        return rendezvous ? rendezvous->registered(room_key(channel(entry.link(group)))) : 0;
    }

    /// Whether someone with the group's next secret is registered under
    /// this one's reset key: its link was reset.
    bool was_reset(const GroupId& group, const GroupEntry& entry) const {
        return rendezvous && rendezvous->registered(reset_key(group, entry.secret)) > 0;
    }

    /// Whether this device meets `group`'s members now: while in its room,
    /// knowing nothing of it yet (joined by its link, nobody met), while
    /// someone is there, to be told who, or while its link is seen reset, to
    /// be given the new one.
    bool meets(const GroupId& group, const GroupEntry& entry) const {
        return !entry.left && (room_group == group || entry.log.empty() ||
                               present(group, entry) > 0 || was_reset(group, entry));
    }

    /// Who is in `group`'s room as told here: each person once, by name.
    std::vector<Group::Here> here_in(const GroupId& group) const {
        std::vector<Group::Here> out;
        const auto told = here.find(group);
        if (told == here.end()) return out;
        for (const auto& [peer, shares] : told->second) {
            const auto who = met.find(peer);
            if (who == met.end()) continue;
            const auto it = std::ranges::find(out, who->second.person, &Group::Here::person);
            if (it != out.end()) {
                it->sharing = it->sharing || shares;
                continue;
            }
            out.push_back(Group::Here{
                .person = who->second.person, .name = who->second.name, .sharing = shares});
        }
        std::ranges::sort(out, [](const Group::Here& a, const Group::Here& b) {
            return std::tie(a.name, a.person) < std::tie(b.name, b.person);
        });
        return out;
    }

    Group group_of(const GroupId& id, const GroupEntry& entry) const {
        const GroupLink link = entry.link(id);
        return Group{.id = id,
                     .name = told_name(entry.log.name()),
                     .link = to_web_link(link),
                     .room = channel(link),
                     .made = entry.key.has_value(),
                     .since = entry.since,
                     .present = present(id, entry),
                     .here = here_in(id)};
    }

    /// The proof of the group's secret to `peer`, and the log if `with_log`
    /// (else theirs asked for).
    void send_group(const PeerId& peer, const GroupId& group, bool with_log) {
        const GroupEntry& entry = book.groups.at(group);
        Out out(Kind::Group);
        out.bytes(proof(entry.secret, "ratcast group", node.local_id(), peer));
        if (with_log) {
            const std::vector<uint8_t> log = encode(entry.log);
            out.u32(log.size());
            out.bytes(log);
        } else {
            out.u32(0);
        }
        // Who else is a member, for a reset to keep them wherever it is made.
        const size_t count = std::min(entry.members.size(), kMostMembersTold);
        out.u16(count);
        size_t written = 0;
        for (const PersonId& member : entry.members) {
            if (written++ == count) break;
            out.bytes(member.key);
        }
        send(peer, out);
    }

    /// The group's secret now and its log, to `peer`, one with the secret
    /// before its reset that the reset keeps.
    void send_rekey(const PeerId& peer, const GroupId& group) {
        const GroupEntry& entry = book.groups.at(group);
        if (!entry.previous) return;
        Out out(Kind::Rekey);
        out.bytes(proof(*entry.previous, "ratcast rekey", node.local_id(), peer));
        out.bytes(entry.secret);
        const std::vector<uint8_t> log = encode(entry.log);
        out.u32(log.size());
        out.bytes(log);
        send(peer, out);
    }

    /// Who proved `group`'s secret, and who is in its room, forgotten: the
    /// secret is another.
    void forget_proofs(const GroupId& group) {
        for (auto& [peer, groups] : members_met) groups.erase(group);
        here.erase(group);
    }

    /// The group's log told to each of its members met: what changed here.
    void tell_members(const GroupId& group) {
        for (const auto& [peer, groups] : members_met)
            if (groups.contains(group)) send_group(peer, group, true);
    }

    /// Whether this device is in `group`'s room, and sharing there, to `peer`.
    void send_here(const PeerId& peer, const GroupId& group) {
        const Where where = room_group != group ? Where::Out : sharing ? Where::Sharing : Where::In;
        Out out(Kind::Here);
        out.bytes(group.key);
        out.u8(uint8_t(where));
        send(peer, out);
    }

    /// The same to each of the group's members met: it changed.
    void tell_here(const GroupId& group) {
        for (const auto& [peer, groups] : members_met)
            if (groups.contains(group)) send_here(peer, group);
    }

    /// The person handed to a device allowed: their key and all they know.
    Out grant() const {
        Out out(Kind::Grant);
        out.bytes(key.seed());
        out.text32(to_json(book));
        return out;
    }

    void update_keys() {
        if (pairing && Steady::now() >= pairing->until && !pairing->asking) pairing.reset();
        if (rendezvous) {
            rendezvous->set_keys(server_keys());
            rendezvous->set_watched(watched());
        }
        if (dht) dht->set_keys(keys());
    }

    void send_presence(const PeerId& peer) {
        Out out(Kind::Presence);
        out.bytes(presence_secret(key));
        send(peer, out);
    }

    void send_ring(const PeerId& peer, const Ring& ring) {
        Out out(Kind::Ring);
        out.bytes(ring.id);
        out.u16(ring.room.size());
        out.bytes(bytes_of(ring.room));
        if (!ring.group.empty()) out.text8(ring.group);
        send(peer, out);
    }

    void send_unring(const PeerId& peer, const RingId& id, Why why) {
        Out out(Kind::Unring);
        out.bytes(id);
        out.u8(uint8_t(why));
        send(peer, out);
    }

    /// The ring made here ended: its friend's devices met told to stop (but
    /// `except`), the key it rang under no longer registered.
    void end_ringing(Why why, const std::optional<PeerId>& except = std::nullopt) {
        if (!ringing) return;
        for (const PeerId& peer : met_of(ringing->ring.person))
            if (peer != except) send_unring(peer, ringing->ring.id, why);
        ringing.reset();
        update_keys();
    }

    /// Whether `person`, a friend, is online: seen at a server, or met.
    bool online(const PersonId& person, const FriendEntry& entry) const {
        if (!met_of(person).empty()) return true;
        return rendezvous && !is_zero(entry.presence) &&
               rendezvous->registered(presence_key(person, entry.presence)) > 0;
    }

    void send(const PeerId& peer, const Out& out) {
        node.send(peer, kChannel, librats::ByteView(out.bytes().data(), out.bytes().size()));
    }

    Out hello() const {
        Out out(Kind::Hello);
        const std::vector<uint8_t> card = encode(card_for(node.local_id()));
        out.u16(card.size());
        out.bytes(card);
        out.text8(book.name);
        return out;
    }

    IdentityCard card_for(const PeerId& device) const {
        return IdentityCard{.certificate = certify(key, device),
                            .request_secret = book.card_secret};
    }

    Out sync() const {
        Out out(Kind::Sync);
        out.text32(to_json(book));
        return out;
    }

    std::vector<PeerId> met_of(const PersonId& person) const {
        std::vector<PeerId> peers;
        for (const auto& [peer, who] : met)
            if (who.person == person) peers.push_back(peer);
        return peers;
    }

    /// Tell the person's other devices what is known now.
    void tell_own() {
        const Out out = sync();
        for (const PeerId& peer : met_of(key.id())) send(peer, out);
    }

    void tell_all_hello() {
        const Out out = hello();
        for (const auto& [peer, who] : met) send(peer, out);
    }

    void send_request(const PeerId& peer, const PersonId& person) {
        const auto it = book.requests.find(person);
        if (it == book.requests.end()) return;
        Out out(Kind::Request);
        out.u8(uint8_t(it->second.via));
        out.bytes(proof(it->second.secret, "ratcast request", node.local_id(), peer));
        send(peer, out);
    }

    void send_answer(const PeerId& peer, const PersonId& person) {
        Out out(Kind::Answer);
        const auto it = book.friends.find(person);
        const bool accepted = it != book.friends.end() && !it->second.removed;
        out.u8(accepted ? 1 : 0);
        if (accepted) out.bytes(it->second.shared);
        send(peer, out);
        if (accepted) send_presence(peer);
    }

    void befriend(const PersonId& person, const std::string& name, const Shared& shared) {
        const uint64_t now = seconds_now();
        FriendEntry& entry = book.friends[person];
        entry.name = name.empty() ? entry.name : name;
        entry.shared = shared;
        entry.since = entry.removed || entry.since == 0 ? now : entry.since;
        entry.removed = false;
        entry.at = std::max(now, entry.at + 1);
        const auto request = book.requests.find(person);
        if (request != book.requests.end()) {
            request->second.gone = true;
            request->second.at = std::max(now, request->second.at + 1);
        }
    }

    bool friends_with(const PersonId& person) const {
        const auto it = book.friends.find(person);
        return it != book.friends.end() && !it->second.removed;
    }

    // ── What comes ───────────────────────────────────────────────────────────

    /// What a message asks of the user or the app, run after the lock.
    struct After {
        bool changed = false;
        std::optional<std::string> device_asks;
        std::optional<bool> joined;
        std::vector<Ring> rung;
        std::vector<std::pair<Ring, RingEnd>> ended;
        std::vector<std::pair<Group, Group::Here>> shares;
    };

    /// Rings past their time: missed. Under the lock.
    void expire(Steady::time_point now, After& after) {
        if (ringing && ringing->until <= now) {
            after.ended.emplace_back(ringing->ring, RingEnd::Missed);
            LOG_INFO("people", ringing->ring.person.tag() << " did not answer");
            end_ringing(Why::Missed);
        }
        for (auto it = incoming.begin(); it != incoming.end();) {
            if (it->second.until > now) {
                ++it;
                continue;
            }
            after.ended.emplace_back(it->second.ring, RingEnd::Missed);
            it = incoming.erase(it);
        }
    }

    /// When the next ring is past its time; none if none rings.
    std::optional<Steady::time_point> next_expiry() const {
        std::optional<Steady::time_point> next;
        if (ringing) next = ringing->until;
        for (const auto& [id, ring] : incoming)
            if (!next || ring.until < *next) next = ring.until;
        return next;
    }

    /// The timer's thread: rings missed as their time comes.
    void keep_time() {
        std::unique_lock lock(mutex);
        while (!stopping) {
            After after;
            expire(Steady::now(), after);
            if (!after.ended.empty()) {
                lock.unlock();
                run(after);
                lock.lock();
                continue;
            }
            if (const auto next = next_expiry()) {
                wake.wait_until(lock, *next);
            } else {
                wake.wait(lock);
            }
        }
    }

    /// An outgoing request told to a device of theirs (on a link, which
    /// carries it or is lost: then it is told again at the next Hello).
    void delivered(const PersonId& person, After& after) {
        const auto it = book.requests.find(person);
        if (it == book.requests.end() || it->second.delivered) return;
        it->second.delivered = true;
        it->second.at = std::max(seconds_now(), it->second.at + 1);
        save();
        tell_own();
        after.changed = true;
    }

    void on_message(const PeerId& peer, std::span<const uint8_t> bytes, After& after) {
        In in(bytes);
        if (in.u8() != kVersion) return;
        const auto kind = Kind(in.u8());
        if (!in.ok()) return;
        if (kind == Kind::Hello) {
            const size_t size = in.u16();
            const std::span<const uint8_t> card_bytes = in.bytes(size);
            const std::string name = told_name(in.text(in.u8()));
            if (!in.ok()) return;
            const std::optional<IdentityCard> card = identity_of(card_bytes, peer);
            if (!card) return;
            on_hello(peer, card->certificate.person, name, after);
            return;
        }
        const auto who = met.find(peer);
        if (who == met.end()) return;  // nothing before its Hello
        const PersonId person = who->second.person;
        switch (kind) {
            case Kind::Request: {
                const auto via = Via(in.u8());
                const auto got = in.array<32>();
                if (!in.ok()) return;
                on_request(peer, person, via, got, after);
                return;
            }
            case Kind::Answer: {
                const bool accepted = in.u8() == 1;
                const Shared shared = accepted ? in.array<32>() : Shared{};
                if (!in.ok()) return;
                on_answer(peer, person, who->second.name, accepted, shared, after);
                return;
            }
            case Kind::Presence: {
                const Secret got = in.array<16>();
                if (!in.ok() || person == key.id() || is_zero(got)) return;
                const auto it = book.friends.find(person);
                if (it == book.friends.end() || it->second.removed || it->second.presence == got)
                    return;
                it->second.presence = got;
                it->second.at = std::max(seconds_now(), it->second.at + 1);
                save();
                update_keys();
                tell_own();
                after.changed = true;
                return;
            }
            case Kind::Ring: {
                const RingId id = in.array<16>();
                const size_t size = in.u16();
                if (!in.ok() || size == 0 || size > kMostRoomLink) return;
                const std::string room = in.text(size);
                // A group's, after it (none from a version before).
                const std::string group = in.done() ? std::string() : told_name(in.text(in.u8()));
                if (!in.ok() || person == key.id() || !friends_with(person) ||
                    incoming.contains(id))
                    return;
                const Ring ring{.id = id,
                                .person = person,
                                .name = book.friends[person].name,
                                .room = room,
                                .group = group,
                                .incoming = true};
                incoming[id] = Incoming{.ring = ring,
                                        .from = peer,
                                        .until = Steady::now() + config.ring_for + kRingGrace};
                wake.notify_all();
                LOG_INFO("people", person.tag() << " rings");
                after.rung.push_back(ring);
                return;
            }
            case Kind::Rung: {
                const RingId id = in.array<16>();
                const bool accepted = in.u8() == 1;
                if (!in.ok()) return;
                if (person == key.id()) {
                    // Answered on another device of this person's.
                    const auto it = incoming.find(id);
                    if (it == incoming.end()) return;
                    after.ended.emplace_back(it->second.ring, RingEnd::AnsweredElsewhere);
                    incoming.erase(it);
                    return;
                }
                if (!ringing || ringing->ring.id != id || ringing->ring.person != person) return;
                after.ended.emplace_back(ringing->ring,
                                         accepted ? RingEnd::Accepted : RingEnd::Declined);
                LOG_INFO("people", person.tag() << (accepted ? " answered" : " declined"));
                end_ringing(Why::AnsweredElsewhere, peer);
                return;
            }
            case Kind::Unring: {
                const RingId id = in.array<16>();
                const auto why = Why(in.u8());
                if (!in.ok()) return;
                const auto it = incoming.find(id);
                if (it == incoming.end() || it->second.ring.person != person) return;
                after.ended.emplace_back(it->second.ring, why == Why::Missed ? RingEnd::Missed
                                                          : why == Why::AnsweredElsewhere
                                                              ? RingEnd::AnsweredElsewhere
                                                              : RingEnd::Cancelled);
                incoming.erase(it);
                return;
            }
            case Kind::Group: {
                const auto got = in.array<32>();
                const size_t size = in.u32();
                if (!in.ok() || size > kMostBook) return;
                const std::span<const uint8_t> log = in.bytes(size);
                // The members it knows, after (none from a version before).
                std::vector<PersonId> members;
                if (in.ok() && !in.done()) {
                    const size_t count = in.u16();
                    if (count > kMostMembersTold) return;
                    for (size_t i = 0; i < count && in.ok(); ++i)
                        members.push_back(PersonId{in.array<32>()});
                }
                if (!in.ok()) return;
                on_group(peer, person, got, log, members, after);
                return;
            }
            case Kind::Rekey: {
                const auto got = in.array<32>();
                const Secret secret = in.array<16>();
                const size_t size = in.u32();
                if (!in.ok() || size > kMostBook) return;
                const std::span<const uint8_t> log = in.bytes(size);
                if (!in.ok()) return;
                on_rekey(peer, person, got, secret, log, after);
                return;
            }
            case Kind::Here: {
                const GroupId group{in.array<32>()};
                const auto where = Where(in.u8());
                if (!in.ok()) return;
                on_here(peer, person, group, where, after);
                return;
            }
            case Kind::Pair: {
                const auto got = in.array<32>();
                const std::string name = told_name(in.text(in.u8()));
                if (!in.ok()) return;
                if (!pairing || Steady::now() >= pairing->until || pairing->asking) return;
                if (!same_secret(got,
                                 proof(pairing->secret, "ratcast pair", peer, node.local_id())))
                    return;
                if (pairing->granted) {
                    if (pairing->granted == peer) send(peer, grant());
                    return;
                }
                pairing->asking = peer;
                LOG_INFO("people", "device " << peer.short_hex() << " asks to be this person");
                after.device_asks = name.empty() ? who->second.name : name;
                return;
            }
            case Kind::Grant: {
                const auto seed = in.array<32>();
                const size_t size = in.u32();
                if (!in.ok() || size > kMostBook) return;
                const std::string text = in.text(size);
                if (!in.ok() || !joining || person != joining->person) return;
                PersonKey granted = PersonKey::from_seed(seed);
                const std::optional<Book> theirs = book_from(text);
                if (granted.id() != joining->person || !theirs) return;
                key = std::move(granted);
                book = *theirs;
                joining.reset();
                save_key();
                save();
                update_keys();
                LOG_INFO("people", "this device is " << key.id().tag() << " now");
                tell_all_hello();
                after.joined = true;
                after.changed = true;
                return;
            }
            case Kind::Refuse:
                if (joining && person == joining->person) {
                    joining.reset();
                    after.joined = false;
                }
                return;
            case Kind::Sync: {
                const size_t size = in.u32();
                if (!in.ok() || size > kMostBook || person != key.id()) return;
                const std::string text = in.text(size);
                if (!in.ok()) return;
                const std::optional<Book> theirs = book_from(text);
                if (theirs && merge(book, *theirs)) {
                    save();
                    update_keys();  // friends, their presence, requests made there
                    after.changed = true;
                }
                return;
            }
            default: return;
        }
    }

    void on_group(const PeerId& peer, const PersonId& person, const std::array<uint8_t, 32>& got,
                  std::span<const uint8_t> bytes, const std::vector<PersonId>& members,
                  After& after) {
        if (person == key.id()) return;  // the person's own devices are told the book
        for (auto& [group, entry] : book.groups) {
            if (entry.left) continue;
            // One with the secret before the link's reset: given this one if
            // the reset keeps them, told nothing if not (the old link alone).
            if (entry.previous &&
                same_secret(got, proof(*entry.previous, "ratcast group", peer, node.local_id()))) {
                const std::optional<GroupReset> reset = entry.log.reset();
                if (reset && std::ranges::find(reset->kept, person) != reset->kept.end()) {
                    LOG_INFO("people",
                             "group " << group.tag() << "'s new link given to " << person.tag());
                    send_rekey(peer, group);
                }
                return;
            }
            if (!same_secret(got, proof(entry.secret, "ratcast group", peer, node.local_id())))
                continue;
            bool known = false;  // anything new kept
            if (!bytes.empty()) {
                const std::optional<GroupLog> theirs = decode_group_log(group, bytes);
                if (theirs && entry.log.merge(group, *theirs)) {
                    LOG_INFO("people", "group " << group.tag() << " caught up by " << person.tag());
                    known = true;
                }
            }
            // A member, as are those they know of.
            known = entry.members.insert(person).second || known;
            for (const PersonId& member : members)
                known = entry.members.insert(member).second || known;
            if (known) {
                save();
                update_keys();  // known now: met for it only in its room
                tell_own();
                after.changed = true;
            }
            // What is known here, once to each member met, and whether this
            // device is in its room.
            bool& told = members_met[peer][group];
            if (!told) {
                told = true;
                send_group(peer, group, true);
                send_here(peer, group);
            }
            return;
        }
    }

    /// A member with the group's secret now gives it, this device having the
    /// one before the reset: kept if the log says it is the group's.
    void on_rekey(const PeerId& peer, const PersonId& person, const std::array<uint8_t, 32>& got,
                  const Secret& secret, std::span<const uint8_t> bytes, After& after) {
        if (person == key.id()) return;
        for (auto& [group, entry] : book.groups) {
            if (entry.left ||
                !same_secret(got, proof(entry.secret, "ratcast rekey", peer, node.local_id())))
                continue;
            const std::optional<GroupLog> theirs = decode_group_log(group, bytes);
            if (!theirs || secret == entry.secret) return;
            GroupLog log = entry.log;
            log.merge(group, *theirs);
            // Only one the maker's last reset marks.
            const std::optional<GroupReset> reset = log.reset();
            if (!reset || reset->mark != secret_mark(GroupLink{group, secret})) return;
            entry.log = std::move(log);
            entry.previous = entry.secret;
            entry.secret = secret;
            // Its members those the reset kept: whoever proved the old secret
            // here since (the old link alone) none.
            entry.members = {reset->kept.begin(), reset->kept.end()};
            entry.at = std::max(seconds_now(), entry.at + 1);
            forget_proofs(group);
            LOG_INFO("people", "group " << group.tag() << "'s link was reset; given the new one by "
                                        << person.tag());
            save();
            update_keys();
            tell_own();
            after.changed = true;
            // A member to them now, by the new secret.
            send_group(peer, group, false);
            return;
        }
    }

    /// A member met is in a group's room, or sharing there, or not.
    void on_here(const PeerId& peer, const PersonId& person, const GroupId& group, Where where,
                 After& after) {
        // Only from one that proved the group's secret.
        const auto proven = members_met.find(peer);
        if (proven == members_met.end() || !proven->second.contains(group)) return;
        const auto sharing_now = [&] {
            for (const Group::Here& who : here_in(group))
                if (who.person == person) return who.sharing;
            return false;
        };
        const bool was_sharing = sharing_now();
        std::map<PeerId, bool>& told = here[group];
        const auto it = told.find(peer);
        if (where == Where::Out) {
            if (it == told.end()) {
                if (told.empty()) here.erase(group);
                return;
            }
            told.erase(it);
            if (told.empty()) here.erase(group);
        } else {
            const bool shares = where == Where::Sharing;
            if (it != told.end() && it->second == shares) return;
            told[peer] = shares;
        }
        after.changed = true;
        if (!was_sharing && sharing_now() && room_group != group) {
            const Group now = group_of(group, book.groups.at(group));
            for (const Group::Here& who : now.here)
                if (who.person == person) after.shares.emplace_back(now, who);
            LOG_INFO("people", person.tag() << " shares in group " << group.tag());
        }
    }

    void on_hello(const PeerId& peer, const PersonId& person, const std::string& name,
                  After& after) {
        const bool first = met_of(person).empty();
        met[peer] = Met{person, name};
        members_met.erase(peer);  // each says its part again
        if (person == key.id()) {
            // The device given the person has it: the link's work is done.
            if (pairing && pairing->granted == peer) {
                pairing.reset();
                update_keys();
            }
            send(peer, sync());
            return;
        }
        // Met, maybe, as a member of a group met for: the secret proven.
        for (const auto& [group, entry] : book.groups)
            if (meets(group, entry)) send_group(peer, group, false);
        if (friends_with(person)) {
            // A friend's device met: theirs, and ours, to be seen online by —
            // with the answer, which their side may still wait for: one lost
            // with a link that lost a race, after they asked as this side did.
            send_answer(peer, person);
            if (first) after.changed = true;
            // Met as it is rung (registered under its ring key, as this is).
            if (ringing && ringing->ring.person == person) send_ring(peer, ringing->ring);
        }
        // Names follow what friends and those asked say now.
        bool renamed = false;
        if (auto it = book.friends.find(person);
            it != book.friends.end() && !name.empty() && it->second.name != name) {
            it->second.name = name;
            renamed = true;
        }
        if (auto it = book.requests.find(person);
            it != book.requests.end() && !name.empty() && it->second.name != name) {
            it->second.name = name;
            renamed = true;
        }
        if (renamed) {
            save();
            after.changed = true;
        }
        const auto request = book.requests.find(person);
        if (request != book.requests.end() && !request->second.incoming &&
            !request->second.declined && !request->second.gone) {
            send_request(peer, person);
            delivered(person, after);
        }
        if (joining && joining->person == person) {
            Out out(Kind::Pair);
            out.bytes(proof(joining->secret, "ratcast pair", node.local_id(), peer));
            out.text8(config.device_name);
            send(peer, out);
        }
    }

    void on_request(const PeerId& peer, const PersonId& person, Via via,
                    const std::array<uint8_t, 32>& got, After& after) {
        if (person == key.id()) return;
        const Secret& secret = via == Via::Card ? book.card_secret : book.link;
        if (!same_secret(got, proof(secret, "ratcast request", peer, node.local_id()))) return;
        if (friends_with(person)) {
            send_answer(peer, person);
            return;
        }
        const uint64_t now = seconds_now();
        auto it = book.requests.find(person);
        if (it != book.requests.end() && !it->second.gone) {
            if (it->second.incoming) {
                if (it->second.declined) send_answer(peer, person);
                return;  // already asked
            }
            if (!it->second.declined) {
                // Both asked: friends, each side's shared secret the same in the
                // end — that of the one with the lower key (on_answer()).
                befriend(person, met[peer].name, random_bytes<32>());
                save();
                update_keys();
                send_answer(peer, person);
                tell_own();
                after.changed = true;
                return;
            }
        }
        RequestEntry& entry = book.requests[person];
        entry = RequestEntry{.name = met[peer].name,
                             .incoming = true,
                             .made = now,
                             .at = std::max(now, entry.at + 1)};
        LOG_INFO("people", person.tag() << " asks to be a friend");
        save();
        tell_own();
        after.changed = true;
    }

    void on_answer(const PeerId& peer, const PersonId& person, const std::string& name,
                   bool accepted, const Shared& shared, After& after) {
        const auto it = book.requests.find(person);
        const bool asked = it != book.requests.end() && !it->second.incoming && !it->second.gone;
        if (friends_with(person)) {
            if (accepted) send_presence(peer);
            // Both asked at once: the lower key's secret.
            if (accepted && person < key.id() && book.friends[person].shared != shared) {
                book.friends[person].shared = shared;
                book.friends[person].at = std::max(seconds_now(), book.friends[person].at + 1);
                save();
                tell_own();
            }
            return;
        }
        if (!asked) return;
        if (accepted) {
            befriend(person, name.empty() ? it->second.name : name, shared);
            send_presence(peer);  // a friend now: told what to watch
            LOG_INFO("people", person.tag() << " accepted");
        } else {
            it->second.declined = true;
            it->second.at = std::max(seconds_now(), it->second.at + 1);
            LOG_INFO("people", person.tag() << " declined");
        }
        save();
        update_keys();
        tell_own();
        after.changed = true;
    }

    /// Ask `person` to be a friend with a secret of theirs; whether anything
    /// changed. Under the lock.
    Result<void> ask(const PersonId& person, Via via, const Secret& secret, std::string name) {
        if (person == key.id() || is_zero(secret)) return fail(Errc::InvalidArgument);
        if (friends_with(person)) return {};
        const uint64_t now = seconds_now();
        RequestEntry& entry = book.requests[person];
        if (entry.incoming && !entry.gone && !entry.declined) {
            // They asked first: this is accepting.
            befriend(person, entry.name, random_bytes<32>());
            for (const PeerId& peer : met_of(person)) send_answer(peer, person);
        } else {
            const std::vector<PeerId> met_now = met_of(person);
            entry = RequestEntry{.name = name.empty() ? entry.name : told_name(std::move(name)),
                                 .incoming = false,
                                 .via = via,
                                 .secret = secret,
                                 .made = now,
                                 .at = std::max(now, entry.at + 1),
                                 .delivered = !met_now.empty()};
            for (const PeerId& peer : met_now) send_request(peer, person);
        }
        save();
        update_keys();
        tell_own();
        return {};
    }

    void run(const After& after) {
        if (after.changed && handlers.changed) handlers.changed();
        if (after.device_asks && handlers.device_asks) handlers.device_asks(*after.device_asks);
        if (after.joined && handlers.joined) handlers.joined(*after.joined);
        if (handlers.rung)
            for (const Ring& ring : after.rung) handlers.rung(ring);
        if (handlers.ring_ended)
            for (const auto& [ring, end] : after.ended) handlers.ring_ended(ring, end);
        if (handlers.shares)
            for (const auto& [group, who] : after.shares) handlers.shares(group, who);
    }

    PeopleConfig config;
    PeopleHandlers handlers;
    mutable std::mutex mutex;
    PersonKey key;
    Book book;
    std::map<PeerId, Met> met;
    std::optional<Pairing> pairing;
    std::optional<PersonLink> joining;
    bool in_room = false;               ///< this device, as set_in_room() says
    std::optional<GroupId> room_group;  ///< the group whose room it is in
    bool sharing = false;               ///< there, as set_sharing() says
    /// Peers met that proved a group's secret, and whether this side told
    /// them its log since they said Hello.
    std::map<PeerId, std::map<GroupId, bool>> members_met;
    /// Of those, the ones that said they are in a group's room: sharing there
    /// or not.
    std::map<GroupId, std::map<PeerId, bool>> here;
    /// The ring this device makes, until it is answered or missed.
    struct Outgoing {
        Ring ring;
        Steady::time_point until;
    };
    std::optional<Outgoing> ringing;
    /// Friends' rings, unanswered: each from the device that rang.
    struct Incoming {
        Ring ring;
        PeerId from;
        Steady::time_point until;
    };
    std::map<RingId, Incoming> incoming;
    std::condition_variable wake;  ///< the timer's: a ring's time changed
    bool stopping = false;
    std::thread timer;
    librats::Node node;
    librats::Rendezvous* rendezvous = nullptr;
    librats::DhtDiscovery* dht = nullptr;
};

Result<std::unique_ptr<People>> People::start(PeopleConfig config, PeopleHandlers handlers) {
    std::unique_ptr<People> people(new People(std::move(config), std::move(handlers)));
    if (!people->state_) return fail(Errc::Malformed);
    State& s = *people->state_;
    if (!s.node.start()) return fail(Errc::Unavailable);
    s.timer = std::thread([&s] { s.keep_time(); });
    LOG_INFO("people", "home node " << s.node.local_id().short_hex() << " up, " << s.key.id().tag()
                                    << ", " << s.book.friends.size() << " friend(s) known");
    return people;
}

People::People(PeopleConfig config, PeopleHandlers handlers) {
    // The person: kept, or new — one broken is never replaced.
    std::optional<PersonKey> key;
    if (config.data_dir.empty()) {
        key = PersonKey::generate();
    } else {
        Result<PersonKey> kept =
            PersonKey::load_or_create(std::filesystem::path(config.data_dir) / kKeyFile);
        if (!kept) {
            LOG_ERROR("people", "the person's key could not be read: " << to_string(kept.error()));
            return;
        }
        key = std::move(*kept);
    }
    std::optional<Book> book;
    if (!config.data_dir.empty()) {
        book = load_book(std::filesystem::path(config.data_dir) / kBookFile);
    }
    const bool fresh = !book;
    if (!book) book = Book::fresh(config.name);
    if (book->name.empty()) book->name = told_name(config.name);

    state_ = std::make_unique<State>(std::move(config), std::move(handlers), std::move(*key),
                                     std::move(*book));
    State& s = *state_;
    if (fresh) s.save();

    KnownServers known(
        s.config.known_servers_file.empty() && !s.config.data_dir.empty()
            ? (std::filesystem::path(s.config.data_dir) / KnownServers::kFile).string()
            : s.config.known_servers_file,
        s.config.built_in_servers);
    librats::Rendezvous::Config meeting;
    meeting.servers = s.config.servers;
    for (const librats::HostEndpoint& at : known.all())
        if (std::ranges::find(meeting.servers, at) == meeting.servers.end())
            meeting.servers.push_back(at);
    meeting.keys = s.server_keys();
    meeting.watched = s.watched();
    s.rendezvous = s.node.add_subsystem(std::make_unique<librats::Rendezvous>(meeting));
    // A friend came online or went, or into a room or out (friends() reads
    // the counts as they are); a group's room no longer empty, or empty: its
    // members there met for, to be told who they are, or no longer.
    s.rendezvous->on_watched([&s](const std::string&, size_t) {
        {
            std::lock_guard lock(s.mutex);
            s.update_keys();
        }
        if (s.handlers.changed) s.handlers.changed();
    });
    s.node.add_subsystem(std::make_unique<librats::HolePunch>());
    s.node.add_subsystem(std::make_unique<librats::Relay>());
    // And through the DHT, with no server, under the same keys — slower (a
    // search a round, every half minute), and two behind NATs that map a port
    // per peer never meet so (no relay).
    if (s.config.dht) {
        const bool internet = s.config.dht_bootstrap.empty();
        if (internet) s.node.add_subsystem(std::make_unique<librats::PortMappingService>());
        librats::DhtDiscovery::Config discovery;
        discovery.data_dir = State::home_dir(s.config);
        discovery.bind_address = s.config.bind_address;
        discovery.bootstrap_nodes = s.config.dht_bootstrap;
        discovery.discover_external_ip = internet;
        discovery.search_interval = s.config.dht_search_interval;
        auto dht = std::make_unique<librats::DhtDiscovery>(std::move(discovery));
        dht->set_keys(s.keys());
        s.dht = s.node.add_subsystem(std::move(dht));
    }

    s.node.on_peer_connected([&s](const librats::Peer& peer) {
        if (s.rendezvous->is_server(peer.id())) return;
        std::lock_guard lock(s.mutex);
        s.send(peer.id(), s.hello());
    });
    // What was said on a link that lost a race of two dialing at once may be
    // lost with it: said again (each Hello has the other say its part again).
    s.node.on_peer_rerouted([&s](const librats::Peer& peer) {
        if (s.rendezvous->is_server(peer.id())) return;
        std::lock_guard lock(s.mutex);
        s.send(peer.id(), s.hello());
    });
    s.node.on_peer_disconnected([&s](const PeerId& peer, librats::CloseReason) {
        bool gone = false;  // a friend's last device met here
        {
            std::lock_guard lock(s.mutex);
            if (const auto it = s.met.find(peer); it != s.met.end()) {
                const PersonId person = it->second.person;
                s.met.erase(it);
                gone = s.friends_with(person) && s.met_of(person).empty();
            }
            if (s.pairing && s.pairing->asking == peer) s.pairing->asking.reset();
            s.members_met.erase(peer);
            // Out of the rooms it was in, as far as is known here.
            for (auto it = s.here.begin(); it != s.here.end();) {
                gone = it->second.erase(peer) > 0 || gone;
                it = it->second.empty() ? s.here.erase(it) : std::next(it);
            }
        }
        if (gone && s.handlers.changed) s.handlers.changed();
    });
    s.node.on(kChannel, [&s](const librats::Peer& peer, librats::ByteView payload) {
        State::After after;
        {
            std::lock_guard lock(s.mutex);
            s.on_message(peer.id(), std::span(payload.data(), payload.size()), after);
        }
        s.run(after);
    });
}

People::~People() {
    if (!state_) return;
    State& s = *state_;
    {
        std::lock_guard lock(s.mutex);
        s.stopping = true;
    }
    s.wake.notify_all();
    if (s.timer.joinable()) s.timer.join();
    s.node.stop();
}

PersonId People::person() const {
    std::lock_guard lock(state_->mutex);
    return state_->key.id();
}

PersonKey People::key() const {
    std::lock_guard lock(state_->mutex);
    return state_->key;
}

IdentityCard People::card(const PeerId& device) const {
    std::lock_guard lock(state_->mutex);
    return state_->card_for(device);
}

std::string People::name() const {
    std::lock_guard lock(state_->mutex);
    return state_->book.name;
}

void People::set_name(std::string name) {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    name = told_name(std::move(name));
    if (name == s.book.name) return;
    s.book.name = std::move(name);
    s.book.name_at = std::max(seconds_now(), s.book.name_at + 1);
    s.save();
    s.tell_all_hello();
    s.tell_own();
}

PeerId People::home() const { return state_->node.local_id(); }

std::string People::home_dir() const { return State::home_dir(state_->config); }

std::string People::link() const {
    std::lock_guard lock(state_->mutex);
    return to_web_link(PersonLink{
        .kind = PersonLink::Kind::Friend, .person = state_->key.id(), .secret = state_->book.link});
}

void People::reset_link() {
    State& s = *state_;
    {
        std::lock_guard lock(s.mutex);
        s.book.link = random_bytes<16>();
        s.book.link_at = std::max(seconds_now(), s.book.link_at + 1);
        s.save();
        s.update_keys();
        s.tell_own();
    }
    if (s.handlers.changed) s.handlers.changed();
}

Result<void> People::add(std::string_view link) {
    const std::optional<PersonLink> parsed = parse_person_link(link);
    if (!parsed || parsed->kind != PersonLink::Kind::Friend) return fail(Errc::InvalidArgument);
    Result<void> asked;
    {
        std::lock_guard lock(state_->mutex);
        asked = state_->ask(parsed->person, Via::Link, parsed->secret, {});
    }
    if (asked && state_->handlers.changed) state_->handlers.changed();
    return asked;
}

Result<void> People::add(const IdentityCard& card, std::string name) {
    if (!is_valid(card.certificate)) return fail(Errc::InvalidArgument);
    Result<void> asked;
    {
        std::lock_guard lock(state_->mutex);
        asked =
            state_->ask(card.certificate.person, Via::Card, card.request_secret, std::move(name));
    }
    if (asked && state_->handlers.changed) state_->handlers.changed();
    return asked;
}

void People::accept(const PersonId& person) {
    State& s = *state_;
    {
        std::lock_guard lock(s.mutex);
        const auto it = s.book.requests.find(person);
        if (it == s.book.requests.end() || !it->second.incoming || it->second.gone) return;
        s.befriend(person, it->second.name, random_bytes<32>());
        for (const PeerId& peer : s.met_of(person)) s.send_answer(peer, person);
        s.save();
        s.tell_own();
    }
    if (s.handlers.changed) s.handlers.changed();
}

void People::decline(const PersonId& person) {
    State& s = *state_;
    {
        std::lock_guard lock(s.mutex);
        const auto it = s.book.requests.find(person);
        if (it == s.book.requests.end() || it->second.gone) return;
        const uint64_t now = seconds_now();
        if (it->second.incoming) {
            it->second.declined = true;
            for (const PeerId& peer : s.met_of(person)) s.send_answer(peer, person);
        } else {
            it->second.gone = true;  // taken back
        }
        it->second.at = std::max(now, it->second.at + 1);
        s.save();
        s.update_keys();
        s.tell_own();
    }
    if (s.handlers.changed) s.handlers.changed();
}

void People::remove(const PersonId& person) {
    State& s = *state_;
    {
        std::lock_guard lock(s.mutex);
        const auto it = s.book.friends.find(person);
        if (it == s.book.friends.end() || it->second.removed) return;
        it->second.removed = true;
        it->second.at = std::max(seconds_now(), it->second.at + 1);
        s.save();
        s.update_keys();  // no longer watched
        s.tell_own();
    }
    if (s.handlers.changed) s.handlers.changed();
}

void People::set_in_room(bool in_room, std::optional<GroupId> group) {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    if (!in_room) group.reset();
    if (s.in_room == in_room && s.room_group == group) return;
    const std::optional<GroupId> before = s.room_group;
    s.in_room = in_room;
    s.room_group = group;
    // The members met of the group's room left, and of the one entered.
    if (before && before != group) s.tell_here(*before);
    if (group && before != group) s.tell_here(*group);
    s.update_keys();  // met there by the group's members in it too
}

void People::set_sharing(bool sharing) {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    if (s.sharing == sharing) return;
    s.sharing = sharing;
    if (s.room_group) s.tell_here(*s.room_group);
}

// ── Groups ──────────────────────────────────────────────────────────────────

Result<Group> People::make_group(std::string name) {
    name = group_name(std::move(name));
    if (name.empty()) return fail(Errc::InvalidArgument);
    State& s = *state_;
    Group made;
    {
        std::lock_guard lock(s.mutex);
        const PersonKey key = PersonKey::generate();
        const GroupId id = GroupId::of(key);
        const uint64_t now = seconds_now();
        GroupEntry& entry = s.book.groups[id];
        entry = GroupEntry{.secret = random_bytes<16>(), .since = now, .at = now};
        entry.key.emplace();
        std::copy(key.seed().begin(), key.seed().end(), entry.key->begin());
        entry.log.append(key, ChangeType::Named, bytes_of(name));
        entry.members.insert(s.key.id());
        s.save();
        s.update_keys();  // its room watched
        s.tell_own();
        made = s.group_of(id, entry);
        LOG_INFO("people", "made group " << id.tag());
    }
    if (s.handlers.changed) s.handlers.changed();
    return made;
}

Result<Group> People::join_group(std::string_view link) {
    const std::optional<GroupLink> parsed = parse_group_link(link);
    if (!parsed) return fail(Errc::InvalidArgument);
    State& s = *state_;
    Group joined;
    {
        std::lock_guard lock(s.mutex);
        GroupEntry& entry = s.book.groups[parsed->group];
        if (entry.at != 0 && !entry.left && entry.secret == parsed->secret)
            return s.group_of(parsed->group, entry);
        const uint64_t now = seconds_now();
        if (entry.at == 0 || entry.left) entry.since = now;
        // Another secret than the one known: kept as the one before, should
        // the log say this is older (a link of before a reset), or not.
        if (entry.at != 0 && entry.secret != parsed->secret) entry.previous = entry.secret;
        entry.secret = parsed->secret;
        entry.left = false;
        entry.settle(parsed->group);
        entry.members.insert(s.key.id());
        entry.at = std::max(now, entry.at + 1);
        s.save();
        s.update_keys();  // its members looked for, its room watched
        s.tell_own();
        joined = s.group_of(parsed->group, entry);
        LOG_INFO("people", "joined group " << parsed->group.tag());
    }
    if (s.handlers.changed) s.handlers.changed();
    return joined;
}

Result<void> People::rename_group(const GroupId& group, std::string name) {
    name = group_name(std::move(name));
    State& s = *state_;
    {
        std::lock_guard lock(s.mutex);
        const auto it = s.book.groups.find(group);
        if (name.empty() || it == s.book.groups.end() || it->second.left || !it->second.key)
            return fail(Errc::InvalidArgument);
        if (it->second.log.name() == name) return {};
        it->second.log.append(PersonKey::from_seed(*it->second.key), ChangeType::Named,
                              bytes_of(name));
        s.save();
        s.tell_own();
        s.tell_members(group);
    }
    if (s.handlers.changed) s.handlers.changed();
    return {};
}

Result<Group> People::reset_group_link(const GroupId& group) {
    State& s = *state_;
    Group reset_now;
    {
        std::lock_guard lock(s.mutex);
        const auto it = s.book.groups.find(group);
        if (it == s.book.groups.end() || it->second.left || !it->second.key)
            return fail(Errc::InvalidArgument);
        GroupEntry& entry = it->second;
        // The members known kept, this person first.
        GroupReset reset;
        reset.kept.push_back(s.key.id());
        for (const PersonId& member : entry.members) {
            if (reset.kept.size() >= GroupReset::kMostKept) break;
            if (member != s.key.id()) reset.kept.push_back(member);
        }
        if (entry.members.size() > reset.kept.size())
            LOG_WARN("people", "group " << group.tag() << " reset keeps " << reset.kept.size()
                                        << " of " << entry.members.size() << " members");
        const Secret secret = random_bytes<16>();
        reset.mark = secret_mark(GroupLink{group, secret});
        entry.log.append(PersonKey::from_seed(*entry.key), ChangeType::Reset, encode(reset));
        // Its members met now, given it at once.
        std::vector<PeerId> told;
        for (const auto& [peer, groups] : s.members_met) {
            const auto who = s.met.find(peer);
            if (groups.contains(group) && who != s.met.end() &&
                std::ranges::find(reset.kept, who->second.person) != reset.kept.end())
                told.push_back(peer);
        }
        entry.previous = entry.secret;
        entry.secret = secret;
        entry.members = {reset.kept.begin(), reset.kept.end()};
        entry.at = std::max(seconds_now(), entry.at + 1);
        s.forget_proofs(group);
        for (const PeerId& peer : told) s.send_rekey(peer, group);
        s.save();
        s.update_keys();
        s.tell_own();
        reset_now = s.group_of(group, entry);
        LOG_INFO("people", "group " << group.tag() << "'s link reset, " << reset.kept.size()
                                    << " member(s) kept");
    }
    if (s.handlers.changed) s.handlers.changed();
    return reset_now;
}

void People::leave_group(const GroupId& group) {
    State& s = *state_;
    {
        std::lock_guard lock(s.mutex);
        const auto it = s.book.groups.find(group);
        if (it == s.book.groups.end() || it->second.left) return;
        it->second.left = true;
        it->second.at = std::max(seconds_now(), it->second.at + 1);
        for (auto& [peer, groups] : s.members_met) groups.erase(group);
        s.here.erase(group);
        s.save();
        s.update_keys();  // neither met for nor watched
        s.tell_own();
        LOG_INFO("people", "left group " << group.tag());
    }
    if (s.handlers.changed) s.handlers.changed();
}

std::vector<Group> People::groups() const {
    std::lock_guard lock(state_->mutex);
    std::vector<Group> out;
    for (const auto& [id, entry] : state_->book.groups)
        if (!entry.left) out.push_back(state_->group_of(id, entry));
    std::ranges::stable_sort(out, [](const Group& a, const Group& b) { return a.name < b.name; });
    return out;
}

std::optional<Group> People::group(const GroupId& group) const {
    std::lock_guard lock(state_->mutex);
    const auto it = state_->book.groups.find(group);
    if (it == state_->book.groups.end() || it->second.left) return std::nullopt;
    return state_->group_of(group, it->second);
}

Result<Ring> People::ring(const PersonId& person, std::string room) {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    const auto it = s.book.friends.find(person);
    // Found at the servers by their ring key, or met already (rung at once).
    if (it == s.book.friends.end() || it->second.removed ||
        (is_zero(it->second.presence) && s.met_of(person).empty()) || room.empty() ||
        room.size() > kMostRoomLink)
        return fail(Errc::InvalidArgument);
    s.end_ringing(Why::HungUp);  // one before
    // To a group's room: its name, for them to know what they join.
    std::string group;
    if (const std::optional<GroupLink> link = parse_group_link(room)) {
        if (const auto kept = s.book.groups.find(link->group); kept != s.book.groups.end())
            group = told_name(kept->second.log.name());
    }
    const Ring ring{.id = random_bytes<16>(),
                    .person = person,
                    .name = it->second.name,
                    .room = std::move(room),
                    .group = std::move(group)};
    s.ringing = State::Outgoing{.ring = ring, .until = Steady::now() + s.config.ring_for};
    s.update_keys();  // registered where their devices are rung
    for (const PeerId& peer : s.met_of(person)) s.send_ring(peer, ring);
    s.wake.notify_all();
    LOG_INFO("people", "ringing " << person.tag());
    return ring;
}

void People::hang_up() {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    s.end_ringing(Why::HungUp);
}

std::optional<Ring> People::ringing() const {
    std::lock_guard lock(state_->mutex);
    if (!state_->ringing) return std::nullopt;
    return state_->ringing->ring;
}

void People::answer(const std::array<uint8_t, 16>& id, bool accept) {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    const auto it = s.incoming.find(id);
    if (it == s.incoming.end()) return;
    Out out(Kind::Rung);
    out.bytes(id);
    out.u8(accept ? 1 : 0);
    // The caller, and this person's other devices, which stop ringing.
    s.send(it->second.from, out);
    for (const PeerId& peer : s.met_of(s.key.id())) s.send(peer, out);
    s.incoming.erase(it);
}

std::vector<Ring> People::rings() const {
    std::lock_guard lock(state_->mutex);
    std::vector<Ring> out;
    for (const auto& [id, ring] : state_->incoming) out.push_back(ring.ring);
    return out;
}

std::vector<Friend> People::friends() const {
    std::lock_guard lock(state_->mutex);
    std::vector<Friend> out;
    for (const auto& [person, entry] : state_->book.friends) {
        if (entry.removed) continue;
        const State& s = *state_;
        const bool in_room = s.rendezvous && !is_zero(entry.presence) &&
                             s.rendezvous->registered(in_room_key(person, entry.presence)) > 0;
        out.push_back(Friend{.person = person,
                             .name = entry.name,
                             .shared = entry.shared,
                             .since = entry.since,
                             .online = in_room || s.online(person, entry),
                             .in_room = in_room});
    }
    std::ranges::sort(out, [](const Friend& a, const Friend& b) { return a.name < b.name; });
    return out;
}

std::vector<FriendRequest> People::requests() const {
    std::lock_guard lock(state_->mutex);
    std::vector<FriendRequest> out;
    for (const auto& [person, entry] : state_->book.requests) {
        if (entry.gone || (entry.incoming && entry.declined)) continue;
        out.push_back(FriendRequest{.person = person,
                                    .name = entry.name,
                                    .incoming = entry.incoming,
                                    .declined = entry.declined,
                                    .delivered = entry.delivered,
                                    .made = entry.made});
    }
    std::ranges::sort(
        out, [](const FriendRequest& a, const FriendRequest& b) { return a.made > b.made; });
    return out;
}

bool People::is_friend(const PersonId& person) const {
    std::lock_guard lock(state_->mutex);
    return state_->friends_with(person);
}

std::string People::device_link() {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    s.pairing = State::Pairing{.secret = random_bytes<16>(),
                               .until = Steady::now() + s.config.device_link_for};
    s.update_keys();
    return to_web_link(PersonLink{
        .kind = PersonLink::Kind::Device, .person = s.key.id(), .secret = s.pairing->secret});
}

void People::cancel_device_link() {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    if (s.pairing && s.pairing->asking) {
        s.send(*s.pairing->asking, Out(Kind::Refuse));
    }
    s.pairing.reset();
    s.update_keys();
}

void People::allow_device(bool allowed) {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    if (!s.pairing || !s.pairing->asking) return;
    const PeerId device = *s.pairing->asking;
    if (allowed) {
        s.send(device, s.grant());
        LOG_INFO("people", "device " << device.short_hex() << " given the person");
        // Kept until it is met as the person, to be given again if it asks.
        s.pairing->asking.reset();
        s.pairing->granted = device;
        return;
    }
    s.send(device, Out(Kind::Refuse));
    s.pairing.reset();
    s.update_keys();
}

Result<void> People::join_as_device(std::string_view link) {
    const std::optional<PersonLink> parsed = parse_person_link(link);
    if (!parsed || parsed->kind != PersonLink::Kind::Device) return fail(Errc::InvalidArgument);
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    if (parsed->person == s.key.id()) return fail(Errc::InvalidArgument);
    s.joining = parsed;
    s.update_keys();
    for (const PeerId& peer : s.met_of(parsed->person)) {
        Out out(Kind::Pair);
        out.bytes(proof(parsed->secret, "ratcast pair", s.node.local_id(), peer));
        out.text8(s.config.device_name);
        s.send(peer, out);
    }
    return {};
}

std::string People::backup() const {
    std::lock_guard lock(state_->mutex);
    return state_->key.to_text() + to_json(state_->book, true) + "\n";
}

Result<void> People::restore(std::string_view backup) {
    const std::optional<PersonKey> key = PersonKey::from_text(backup);
    if (!key) return fail(Errc::InvalidArgument);
    // What the person knew, after the key's two lines.
    std::optional<Book> theirs;
    size_t lines = 0;
    size_t at = 0;
    while (lines < 2 && at != std::string_view::npos) {
        at = backup.find('\n', at);
        if (at != std::string_view::npos) ++at;
        ++lines;
    }
    if (at != std::string_view::npos) theirs = book_from(backup.substr(at));
    State& s = *state_;
    {
        std::lock_guard lock(s.mutex);
        s.key = *key;
        if (theirs) {
            merge(*theirs, s.book);  // what this device knew too
            s.book = *theirs;
        }
        s.save_key();
        s.save();
        s.update_keys();
        s.tell_all_hello();
    }
    if (s.handlers.changed) s.handlers.changed();
    return {};
}

}  // namespace ratcast
