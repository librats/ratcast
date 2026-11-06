#include "book.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include "librats/util/json.h"

namespace ratcast {

uint64_t seconds_now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
}

bool is_zero(std::span<const uint8_t> bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; });
}

std::string told_name(std::string name) {
    std::erase_if(name, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
    if (name.size() > 128) name.resize(128);
    return name;
}

std::string group_name(std::string name) {
    name = told_name(std::move(name));
    const size_t first = name.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return name.substr(first, name.find_last_not_of(' ') - first + 1);
}

Book Book::fresh(std::string name) {
    const uint64_t now = seconds_now();
    return Book{.name = told_name(std::move(name)),
                .name_at = 0,
                .link = random_bytes<16>(),
                .link_at = now,
                .card_secret = random_bytes<16>(),
                .card_secret_at = now};
}

namespace {

librats::Json json_of(const Book& book) {
    librats::Json json = librats::Json::object();
    json["version"] = 1;
    json["name"] = book.name;
    json["name_at"] = book.name_at;
    json["link"] = to_base64url(book.link);
    json["link_at"] = book.link_at;
    // As it was first called: the room's secret, the card's now.
    json["room"] = to_base64url(book.card_secret);
    json["room_at"] = book.card_secret_at;
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
    librats::Json groups = librats::Json::array();
    for (const auto& [group, entry] : book.groups) {
        librats::Json g = librats::Json::object();
        g["group"] = group.to_hex();
        g["secret"] = to_base64url(entry.secret);
        if (entry.previous) g["previous"] = to_base64url(*entry.previous);
        if (entry.key) g["key"] = to_base64url(*entry.key);
        g["log"] = to_base64url(encode(entry.log));
        librats::Json members = librats::Json::array();
        for (const PersonId& member : entry.members) members.push_back(member.to_hex());
        g["members"] = std::move(members);
        g["since"] = entry.since;
        g["at"] = entry.at;
        g["left"] = entry.left;
        groups.push_back(std::move(g));
    }
    json["groups"] = std::move(groups);
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

}  // namespace

std::string to_json(const Book& book, bool pretty) {
    return pretty ? json_of(book).dump(2) : json_of(book).dump();
}

std::optional<Book> book_from(std::string_view text) {
    const librats::Json json = librats::Json::parse(std::string(text), nullptr, false);
    if (json.is_discarded() || !json.is_object()) return std::nullopt;
    Book book;
    book.name = read_text(json, "name");
    book.name_at = read_u64(json, "name_at");
    if (!read_secret(json, "link", book.link) || !read_secret(json, "room", book.card_secret))
        return std::nullopt;
    book.link_at = read_u64(json, "link_at");
    book.card_secret_at = read_u64(json, "room_at");
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
            entry.via = read_u64(r, "via") == 1 ? Via::Card : Via::Link;
            read_secret(r, "secret", entry.secret);
            entry.made = read_u64(r, "made");
            entry.at = read_u64(r, "at");
            entry.gone = read_bool(r, "gone");
            entry.delivered = read_bool(r, "delivered");
            book.requests[*person] = std::move(entry);
        }
    }
    if (json.contains("groups") && json["groups"].is_array()) {
        for (const librats::Json& g : json["groups"]) {
            if (!g.is_object() || !g.contains("group") || !g["group"].is_string()) continue;
            const std::optional<GroupId> group = GroupId::from_hex(g["group"].get<std::string>());
            GroupEntry entry;
            if (!group || !read_secret(g, "secret", entry.secret)) continue;
            if (Secret previous{}; read_secret(g, "previous", previous)) entry.previous = previous;
            if (std::array<uint8_t, 32> seed{}; read_secret(g, "key", seed)) {
                // Only the group's own.
                if (GroupId::of(PersonKey::from_seed(seed)) == *group) entry.key = seed;
            }
            if (g.contains("log") && g["log"].is_string()) {
                const std::string written = g["log"].get<std::string>();
                const size_t size = written.size() * 3 / 4;  // as base64url has it, unpadded
                if (const std::optional<std::vector<uint8_t>> bytes =
                        from_base64url(written, size)) {
                    if (std::optional<GroupLog> log = decode_group_log(*group, *bytes))
                        entry.log = std::move(*log);
                }
            }
            if (g.contains("members") && g["members"].is_array()) {
                for (const librats::Json& member : g["members"]) {
                    if (!member.is_string()) continue;
                    if (const std::optional<PersonId> person =
                            PersonId::from_hex(member.get<std::string>()))
                        entry.members.insert(*person);
                }
            }
            entry.since = read_u64(g, "since");
            entry.at = read_u64(g, "at");
            entry.left = read_bool(g, "left");
            book.groups[*group] = std::move(entry);
        }
    }
    return book;
}

bool GroupEntry::settle(const GroupId& group) {
    if (!previous || log.current(link(group)) || !log.current(GroupLink{group, *previous}))
        return false;
    std::swap(secret, *previous);
    return true;
}

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
    if (theirs.card_secret_at > mine.card_secret_at) {
        mine.card_secret = theirs.card_secret;
        mine.card_secret_at = theirs.card_secret_at;
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
    for (const auto& [group, entry] : theirs.groups) {
        const auto it = mine.groups.find(group);
        if (it == mine.groups.end()) {
            mine.groups[group] = entry;
            changed = true;
            continue;
        }
        GroupEntry& kept = it->second;
        // The secret the other had, should the log say it is the group's.
        const Secret mine_before = kept.secret;
        if (entry.at > kept.at) {
            kept.secret = entry.secret;
            kept.previous = entry.previous;
            kept.since = entry.since;
            kept.at = entry.at;
            kept.left = entry.left;
            changed = true;
        }
        if (!kept.key && entry.key) {
            kept.key = entry.key;
            changed = true;
        }
        // The log and the members whatever: what one device was told, the
        // others know.
        changed = kept.log.merge(group, entry.log) || changed;
        for (const PersonId& member : entry.members)
            changed = kept.members.insert(member).second || changed;
        // Of the secrets the two had, the group's: a reset made or told on
        // one device stands on all, whichever changed last.
        for (const Secret& other : {mine_before, entry.secret}) {
            if (other == kept.secret || kept.log.current(GroupLink{group, kept.secret}) ||
                !kept.log.current(GroupLink{group, other}))
                continue;
            kept.previous = kept.secret;
            kept.secret = other;
            changed = true;
        }
        changed = kept.settle(group) || changed;
    }
    return changed;
}

std::optional<Book> load_book(const std::filesystem::path& path) {
    const std::optional<std::string> text = read_file(path);
    return text ? book_from(*text) : std::nullopt;
}

Result<void> save_book(const std::filesystem::path& path, const Book& book) {
    return write_file(path, to_json(book, true));
}

}  // namespace ratcast
