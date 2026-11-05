#include "ratcast/client/people.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>

#include "ratcast/client/known_servers.h"

#include "librats/node/node.h"
#include "librats/subsystems/dht_discovery.h"
#include "librats/subsystems/hole_punch.h"
#include "librats/subsystems/port_mapping_service.h"
#include "librats/subsystems/relay.h"
#include "librats/subsystems/rendezvous.h"
#include "librats/util/json.h"
#include "librats/util/logger.h"

namespace ratcast {
namespace {

using Steady = std::chrono::steady_clock;
using Secret = std::array<uint8_t, 16>;
using Shared = std::array<uint8_t, 32>;
using RingId = std::array<uint8_t, 16>;

constexpr std::string_view kChannel = "ratcast/people";
constexpr std::string_view kKeyFile = "person.key";
constexpr std::string_view kBookFile = "people.json";
constexpr std::string_view kHomeDir = "home";

constexpr std::string_view kFriendPage = "https://ratcast.app/u/#";
constexpr std::string_view kDevicePage = "https://ratcast.app/d/#";
constexpr std::string_view kFriendScheme = "rats://u/";
constexpr std::string_view kDeviceScheme = "rats://d/";

// ── The wire ────────────────────────────────────────────────────────────────
//
// On the home node's channel, each message: u8 version, u8 kind, then
//   Hello    u16 card length + IdentityCard, u8 name length + name
//   Request  u8 via (0 the person's link, 1 their room secret), 32-byte proof
//   Answer   u8 accepted, [32-byte shared secret]
//   Pair     32-byte proof, u8 name length + the device's name
//   Grant    32-byte seed, u32 length + the book (JSON)
//   Refuse
//   Sync     u32 length + the book (JSON)
//   Presence 16-byte secret: the sender's person's presence secret, to a friend
//   Ring     16-byte id, u16 length + the room's link: a friend rings
//   Rung     16-byte id, u8 accepted: a ring answered — to the caller, and to
//            the answering person's other devices
//   Unring   16-byte id, u8 why (0 hung up, 1 missed, 2 answered elsewhere)
// A proof is the MAC of what it proves under the secret: "ratcast request" or
// "ratcast pair", the sender's peer id, the receiver's.

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
    Unring
};
enum class Why : uint8_t { HungUp = 0, Missed = 1, AnsweredElsewhere = 2 };

/// The longest room link a ring carries.
constexpr size_t kMostRoomLink = 4096;
/// How much longer a ring rings where it is rung than where it is made: the
/// caller's word that it was missed comes first.
constexpr auto kRingGrace = std::chrono::seconds(5);
enum class Via : uint8_t { Link = 0, Room = 1 };

constexpr size_t kMostBook = 1 << 20;

class Out {
public:
    explicit Out(Kind kind) : bytes_{kVersion, uint8_t(kind)} {}
    void u8(uint8_t value) { bytes_.push_back(value); }
    void u16(size_t value) {
        bytes_.push_back(uint8_t(value >> 8));
        bytes_.push_back(uint8_t(value));
    }
    void u32(size_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) bytes_.push_back(uint8_t(value >> shift));
    }
    void bytes(std::span<const uint8_t> bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }
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

private:
    std::vector<uint8_t> bytes_;
};

class In {
public:
    explicit In(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    bool ok() const { return ok_; }
    uint8_t u8() {
        if (!need(1)) return 0;
        return bytes_[at_++];
    }
    size_t u16() {
        const size_t high = u8();
        return (high << 8) | u8();
    }
    size_t u32() {
        size_t value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 8) | u8();
        return value;
    }
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
    bool need(size_t count) {
        if (!ok_ || bytes_.size() - at_ < count) ok_ = false;
        return ok_;
    }

    std::span<const uint8_t> bytes_;
    size_t at_ = 0;
    bool ok_ = true;
};

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
    return "ratcast/0/devices/" +
           to_hex(mac(key.seed(),
                      std::span(reinterpret_cast<const uint8_t*>(what.data()), what.size())));
}

/// The secret a person is seen online by, told to friends: of their key's, the
/// same on each of their devices.
Secret presence_secret(const PersonKey& key) {
    constexpr std::string_view what = "ratcast presence";
    const auto full =
        mac(key.seed(), std::span(reinterpret_cast<const uint8_t*>(what.data()), what.size()));
    Secret out{};
    std::copy_n(full.begin(), out.size(), out.begin());
    return out;
}

/// The key a person's devices are registered under, for friends to watch;
/// and the one they are under while in a room.
std::string presence_key(const PersonId& person, const Secret& secret) {
    return "ratcast/0/online/" + to_hex(mac(secret, person.key));
}
std::string room_key(const PersonId& person, const Secret& secret) {
    return "ratcast/0/in-room/" + to_hex(mac(secret, person.key));
}
/// The key a person's devices are rung under: a caller registered there
/// while it rings is introduced to them, and nobody watching is misled.
std::string ring_key(const PersonId& person, const Secret& secret) {
    return "ratcast/0/ring/" + to_hex(mac(secret, person.key));
}

uint64_t seconds_now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
}

bool is_zero(std::span<const uint8_t> bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; });
}

/// A name as it may be told: plain, at most a Presence's.
std::string told_name(std::string name) {
    std::erase_if(name, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
    if (name.size() > 128) name.resize(128);
    return name;
}

// ── The book: what the person knows, the same on each of their devices ──────

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

struct Book {
    std::string name;
    uint64_t name_at = 0;
    Secret link{};
    uint64_t link_at = 0;
    Secret room{};
    uint64_t room_at = 0;
    std::map<PersonId, FriendEntry> friends;
    std::map<PersonId, RequestEntry> requests;

    static Book fresh(std::string name) {
        const uint64_t now = seconds_now();
        return Book{.name = told_name(std::move(name)),
                    .name_at = 0,
                    .link = random_bytes<16>(),
                    .link_at = now,
                    .room = random_bytes<16>(),
                    .room_at = now};
    }
};

librats::Json to_json(const Book& book) {
    librats::Json json = librats::Json::object();
    json["version"] = 1;
    json["name"] = book.name;
    json["name_at"] = book.name_at;
    json["link"] = to_base64url(book.link);
    json["link_at"] = book.link_at;
    json["room"] = to_base64url(book.room);
    json["room_at"] = book.room_at;
    librats::Json friends = librats::Json::array();
    for (const auto& [person, entry] : book.friends) {
        librats::Json f = librats::Json::object();
        f["person"] = person.to_hex();
        f["name"] = entry.name;
        f["shared"] = to_base64url(entry.shared);
        if (!is_zero(entry.presence)) f["presence"] = to_base64url(entry.presence);
        f["since"] = entry.since;
        f["at"] = entry.at;
        f["removed"] = entry.removed;
        friends.push_back(std::move(f));
    }
    json["friends"] = std::move(friends);
    librats::Json requests = librats::Json::array();
    for (const auto& [person, entry] : book.requests) {
        librats::Json r = librats::Json::object();
        r["person"] = person.to_hex();
        r["name"] = entry.name;
        r["incoming"] = entry.incoming;
        r["declined"] = entry.declined;
        r["via"] = int(entry.via);
        r["secret"] = to_base64url(entry.secret);
        r["made"] = entry.made;
        r["at"] = entry.at;
        r["gone"] = entry.gone;
        r["delivered"] = entry.delivered;
        requests.push_back(std::move(r));
    }
    json["requests"] = std::move(requests);
    return json;
}

template <size_t N>
bool read_secret(const librats::Json& json, const char* key, std::array<uint8_t, N>& out) {
    if (!json.contains(key) || !json[key].is_string()) return false;
    const std::optional<std::vector<uint8_t>> bytes =
        from_base64url(json[key].get<std::string>(), N);
    if (!bytes) return false;
    std::copy(bytes->begin(), bytes->end(), out.begin());
    return true;
}

uint64_t read_u64(const librats::Json& json, const char* key) {
    if (!json.contains(key) || !json[key].is_number_integer()) return 0;
    return json[key].get<uint64_t>();
}

bool read_bool(const librats::Json& json, const char* key) {
    return json.contains(key) && json[key].is_boolean() && json[key].get<bool>();
}

std::string read_text(const librats::Json& json, const char* key) {
    if (!json.contains(key) || !json[key].is_string()) return {};
    return told_name(json[key].get<std::string>());
}

std::optional<Book> book_from(std::string_view text) {
    const librats::Json json = librats::Json::parse(std::string(text), nullptr, false);
    if (json.is_discarded() || !json.is_object()) return std::nullopt;
    Book book;
    book.name = read_text(json, "name");
    book.name_at = read_u64(json, "name_at");
    if (!read_secret(json, "link", book.link) || !read_secret(json, "room", book.room))
        return std::nullopt;
    book.link_at = read_u64(json, "link_at");
    book.room_at = read_u64(json, "room_at");
    if (json.contains("friends") && json["friends"].is_array()) {
        for (const librats::Json& f : json["friends"]) {
            if (!f.is_object() || !f.contains("person") || !f["person"].is_string()) continue;
            const std::optional<PersonId> person =
                PersonId::from_hex(f["person"].get<std::string>());
            FriendEntry entry;
            if (!person || !read_secret(f, "shared", entry.shared)) continue;
            entry.name = read_text(f, "name");
            read_secret(f, "presence", entry.presence);
            entry.since = read_u64(f, "since");
            entry.at = read_u64(f, "at");
            entry.removed = read_bool(f, "removed");
            book.friends[*person] = std::move(entry);
        }
    }
    if (json.contains("requests") && json["requests"].is_array()) {
        for (const librats::Json& r : json["requests"]) {
            if (!r.is_object() || !r.contains("person") || !r["person"].is_string()) continue;
            const std::optional<PersonId> person =
                PersonId::from_hex(r["person"].get<std::string>());
            if (!person) continue;
            RequestEntry entry;
            entry.name = read_text(r, "name");
            entry.incoming = read_bool(r, "incoming");
            entry.declined = read_bool(r, "declined");
            entry.via = read_u64(r, "via") == 1 ? Via::Room : Via::Link;
            read_secret(r, "secret", entry.secret);
            entry.made = read_u64(r, "made");
            entry.at = read_u64(r, "at");
            entry.gone = read_bool(r, "gone");
            entry.delivered = read_bool(r, "delivered");
            book.requests[*person] = std::move(entry);
        }
    }
    return book;
}

/// What `theirs` knows that `mine` does not, taken in: whatever was changed
/// last wins. Whether anything was.
bool merge(Book& mine, const Book& theirs) {
    bool changed = false;
    if (theirs.name_at > mine.name_at) {
        mine.name = theirs.name;
        mine.name_at = theirs.name_at;
        changed = true;
    }
    if (theirs.link_at > mine.link_at) {
        mine.link = theirs.link;
        mine.link_at = theirs.link_at;
        changed = true;
    }
    if (theirs.room_at > mine.room_at) {
        mine.room = theirs.room;
        mine.room_at = theirs.room_at;
        changed = true;
    }
    for (const auto& [person, entry] : theirs.friends) {
        const auto it = mine.friends.find(person);
        if (it == mine.friends.end() || entry.at > it->second.at) {
            mine.friends[person] = entry;
            changed = true;
        }
    }
    for (const auto& [person, entry] : theirs.requests) {
        const auto it = mine.requests.find(person);
        if (it == mine.requests.end() || entry.at > it->second.at) {
            mine.requests[person] = entry;
            changed = true;
        }
    }
    return changed;
}

Result<void> write_file(const std::filesystem::path& path, std::string_view text) {
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::path written = path;
    written += ".new";
    {
        std::ofstream file(written, std::ios::binary | std::ios::trunc);
        if (!file) return fail(Errc::Unavailable);
        file << text;
        if (!file.flush()) return fail(Errc::Unavailable);
    }
    std::filesystem::rename(written, path, error);
    if (error) return fail(Errc::Unavailable);
    return {};
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::optional<PersonLink> link_after(std::string_view rest, PersonLink::Kind kind) {
    // An escaped '.' from a messenger: "%2E".
    std::string text(rest);
    for (size_t at = text.find("%2"); at != std::string::npos; at = text.find("%2", at)) {
        if (at + 2 < text.size() && (text[at + 2] == 'E' || text[at + 2] == 'e')) {
            text.replace(at, 3, ".");
        } else {
            ++at;
        }
    }
    while (!text.empty() && (text.back() == '/' || text.back() == ' ')) text.pop_back();
    const size_t dot = text.find('.');
    if (dot == std::string::npos) return std::nullopt;
    const std::optional<std::vector<uint8_t>> person =
        from_base64url(std::string_view(text).substr(0, dot), 32);
    const std::optional<std::vector<uint8_t>> secret =
        from_base64url(std::string_view(text).substr(dot + 1), 16);
    if (!person || !secret) return std::nullopt;
    PersonLink link{.kind = kind};
    std::copy(person->begin(), person->end(), link.person.key.begin());
    std::copy(secret->begin(), secret->end(), link.secret.begin());
    return link;
}

}  // namespace

// ── Links ───────────────────────────────────────────────────────────────────

std::optional<PersonLink> parse_person_link(std::string_view link) {
    while (!link.empty() && link.front() == ' ') link.remove_prefix(1);
    const auto starts = [&](std::string_view prefix) {
        if (link.size() < prefix.size()) return false;
        return std::equal(prefix.begin(), prefix.end(), link.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    };
    for (const auto& [prefix, kind] : {std::pair{kFriendPage, PersonLink::Kind::Friend},
                                       std::pair{kDevicePage, PersonLink::Kind::Device},
                                       std::pair{kFriendScheme, PersonLink::Kind::Friend},
                                       std::pair{kDeviceScheme, PersonLink::Kind::Device}}) {
        if (starts(prefix)) return link_after(link.substr(prefix.size()), kind);
        // The website's without its www., or over http.
    }
    for (const std::string_view other :
         {"https://www.ratcast.app/", "http://ratcast.app/", "http://www.ratcast.app/"}) {
        if (!starts(other)) continue;
        const std::string_view rest = link.substr(other.size());
        if (rest.starts_with("u/#")) return link_after(rest.substr(3), PersonLink::Kind::Friend);
        if (rest.starts_with("d/#")) return link_after(rest.substr(3), PersonLink::Kind::Device);
    }
    return std::nullopt;
}

std::string to_web_link(const PersonLink& link) {
    return std::string(link.kind == PersonLink::Kind::Friend ? kFriendPage : kDevicePage) +
           to_base64url(link.person.key) + "." + to_base64url(link.secret);
}

std::string to_string(const PersonLink& link) {
    return std::string(link.kind == PersonLink::Kind::Friend ? kFriendScheme : kDeviceScheme) +
           to_base64url(link.person.key) + "." + to_base64url(link.secret);
}

// ── People ──────────────────────────────────────────────────────────────────

struct People::State {
    /// A device link made here.
    struct Pairing {
        Secret secret{};
        Steady::time_point until{};
        std::optional<PeerId> asking;  ///< the device that proved it, waiting for an answer
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

    static librats::NodeConfig node_config(const PeopleConfig& config) {
        librats::NodeConfig node;
        node.listen_port = config.port;
        node.bind_address = config.bind_address;
        node.data_dir = config.data_dir.empty()
                            ? std::string()
                            : (std::filesystem::path(config.data_dir) / kHomeDir).string();
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
        if (!write_file(file(kBookFile), to_json(book).dump(2)))
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
                                         meeting_key(key.id(), book.room), devices_key(key)};
        if (pairing && Steady::now() < pairing->until)
            keys.push_back(meeting_key(key.id(), pairing->secret));
        if (joining) keys.push_back(meeting_key(joining->person, joining->secret));
        for (const auto& [person, request] : book.requests) {
            if (!request.incoming && !request.declined && !request.gone)
                keys.push_back(meeting_key(person, request.secret));
        }
        return keys;
    }

    /// What is registered at servers: those, and the person's presence key —
    /// their room key too while in a room — last: a server that keeps fewer
    /// keys is an older one, which nobody watches at. Not in the DHT: it has
    /// nothing to watch with (L26), and a friend not met there is not seen.
    std::vector<std::string> server_keys() const {
        std::vector<std::string> all = keys();
        const Secret own = presence_secret(key);
        all.push_back(presence_key(key.id(), own));
        all.push_back(ring_key(key.id(), own));
        if (in_room) all.push_back(room_key(key.id(), own));
        // While ringing a friend: where their devices are.
        if (ringing) {
            const auto it = book.friends.find(ringing->ring.person);
            if (it != book.friends.end() && !is_zero(it->second.presence))
                all.push_back(ring_key(ringing->ring.person, it->second.presence));
        }
        return all;
    }

    /// What is watched at servers: each friend's presence and room keys.
    std::vector<std::string> watched() const {
        std::vector<std::string> out;
        for (const auto& [person, entry] : book.friends) {
            if (entry.removed || is_zero(entry.presence)) continue;
            out.push_back(presence_key(person, entry.presence));
            out.push_back(room_key(person, entry.presence));
        }
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
        out.bytes(std::span(reinterpret_cast<const uint8_t*>(ring.room.data()), ring.room.size()));
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
        return IdentityCard{.certificate = certify(key, device), .request_secret = book.room};
    }

    Out sync() const {
        Out out(Kind::Sync);
        out.text32(to_json(book).dump());
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
                if (!in.ok() || person == key.id() || !friends_with(person) ||
                    incoming.contains(id))
                    return;
                const Ring ring{.id = id,
                                .person = person,
                                .name = book.friends[person].name,
                                .room = room,
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
            case Kind::Pair: {
                const auto got = in.array<32>();
                const std::string name = told_name(in.text(in.u8()));
                if (!in.ok()) return;
                if (!pairing || Steady::now() >= pairing->until || pairing->asking) return;
                if (!same_secret(got,
                                 proof(pairing->secret, "ratcast pair", peer, node.local_id())))
                    return;
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

    void on_hello(const PeerId& peer, const PersonId& person, const std::string& name,
                  After& after) {
        const bool first = met_of(person).empty();
        met[peer] = Met{person, name};
        if (person == key.id()) {
            send(peer, sync());
            return;
        }
        if (friends_with(person)) {
            // A friend's device met: theirs, and ours, to be seen online by.
            send_presence(peer);
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
        const Secret& secret = via == Via::Room ? book.room : book.link;
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
    }

    PeopleConfig config;
    PeopleHandlers handlers;
    mutable std::mutex mutex;
    PersonKey key;
    Book book;
    std::map<PeerId, Met> met;
    std::optional<Pairing> pairing;
    std::optional<PersonLink> joining;
    bool in_room = false;  ///< this device, as set_in_room() says
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
        if (const std::optional<std::string> text =
                read_file(std::filesystem::path(config.data_dir) / kBookFile))
            book = book_from(*text);
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
    // the counts as they are).
    s.rendezvous->on_watched([&s](const std::string&, size_t) {
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
        discovery.data_dir = State::node_config(s.config).data_dir;
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
            state_->ask(card.certificate.person, Via::Room, card.request_secret, std::move(name));
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

void People::set_in_room(bool in_room) {
    State& s = *state_;
    std::lock_guard lock(s.mutex);
    if (s.in_room == in_room) return;
    s.in_room = in_room;
    s.update_keys();
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
    const Ring ring{.id = random_bytes<16>(),
                    .person = person,
                    .name = it->second.name,
                    .room = std::move(room)};
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
                             s.rendezvous->registered(room_key(person, entry.presence)) > 0;
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
        Out out(Kind::Grant);
        out.bytes(s.key.seed());
        out.text32(to_json(s.book).dump());
        s.send(device, out);
        LOG_INFO("people", "device " << device.short_hex() << " given the person");
    } else {
        s.send(device, Out(Kind::Refuse));
    }
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
    return state_->key.to_text() + to_json(state_->book).dump(2) + "\n";
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
