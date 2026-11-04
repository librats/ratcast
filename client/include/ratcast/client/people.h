#pragma once

/**
 * @file people.h
 * @brief The people one knows: one's own devices, and friends.
 *
 * People is this device's home node: a librats node of its own (its identity
 * kept in the data directory's "home"), running for as long as the app does,
 * met at servers (librats' Rendezvous) under keys only those who should meet
 * it can work out. The person's key (identity.h) is kept in the data
 * directory, "person.key"; what the person knows — their name, their friends,
 * the requests both ways, their links' secrets — in "people.json", the same
 * on each of their devices, which tell each other of it as they meet.
 *
 * A friend is added by a request, which the other accepts or declines:
 * - by the person's link (link(): https://ratcast.app/u/#PERSON.SECRET), the
 *   secret in it reset at will (reset_link(): the old one refused from then
 *   on, friends kept);
 * - from a room, by a member's identity card (IdentityCard::request_secret,
 *   card(): the person's room secret, which every room member is told).
 * Whoever asks is met under the key of the person and the secret, sends a
 * proof of the secret, and is told whether they were accepted when both are
 * online; until then their request waits, asked again as they meet. Nothing
 * reaches the person from anyone without a secret of theirs.
 *
 * A device is added from one that has the person's key: device_link() (a
 * secret for 10 minutes: https://ratcast.app/d/#PERSON.SECRET, shown as a QR
 * code) opened on the new device (join_as_device()), which proves it; the
 * one that made it asks the user (PeopleHandlers::device_asks), and given the
 * leave (allow_device()) hands the person's key and all they know over Noise.
 * The new device is the person from then on.
 *
 * Friends see each other online (Friend::online, in_room): each device of a
 * person registers at the servers under their presence key — of a secret of
 * the person's key, told to each friend as they become one or meet — and
 * under its room key while in a room; each watches its friends' (librats'
 * Rendezvous::set_watched()), told how many are there, meeting none of them.
 * With no server, a friend is online only while met.
 *
 * Threads: every method may be called from any thread. Handlers run on the
 * network's threads, never under People's lock, and must not block or
 * destroy it.
 */

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ratcast/client/identity.h"
#include "ratcast/core/result.h"
#include "ratcast/ratcast.h"

#include "librats/core/host_endpoint.h"

namespace ratcast {

/// A link to a person — to ask to be their friend, or for a device of theirs
/// to become them — as a rats:// link or the website's.
struct PersonLink {
    enum class Kind : uint8_t { Friend, Device };
    Kind kind = Kind::Friend;
    PersonId person;
    std::array<uint8_t, 16> secret{};

    friend bool operator==(const PersonLink&, const PersonLink&) = default;
};

/// "rats://u/PERSON.SECRET" (a friend's), "rats://d/…" (a device's), or the
/// website's "https://ratcast.app/u/#…", "/d/#…"; nullopt for anything else.
std::optional<PersonLink> parse_person_link(std::string_view link);
/// The website's link.
std::string to_web_link(const PersonLink& link);
/// The rats:// link.
std::string to_string(const PersonLink& link);

struct Friend {
    PersonId person;
    std::string name;  ///< as they last said it
    /// A secret the two share, for what friends do later.
    std::array<uint8_t, 32> shared{};
    uint64_t since = 0;  ///< seconds since the Unix epoch
    /// A device of theirs is online: seen at a server, or met here.
    bool online = false;
    /// A device of theirs is in a room (People::set_in_room()), as seen at a
    /// server.
    bool in_room = false;
};

struct FriendRequest {
    PersonId person;
    std::string name;  ///< theirs, once met; "" before
    /// Theirs to this person (to accept or decline), or this person's to
    /// them, waiting for an answer.
    bool incoming = false;
    /// An outgoing one they declined.
    bool declined = false;
    /// An outgoing one told to a device of theirs: not looked for any more.
    bool delivered = false;
    uint64_t made = 0;
};

struct PeopleConfig {
    /// Where the person's key, what they know and the home node's identity
    /// are kept; "": nowhere, a new person each run (tests).
    std::string data_dir;
    /// The servers met at: those given, and those KnownServers keeps in the
    /// data directory ("servers"), after the build's.
    std::vector<librats::HostEndpoint> servers = {};
    std::vector<librats::HostEndpoint> built_in_servers = {};
    std::string known_servers_file = {};
    /// The name told to those met, until set_name().
    std::string name;
    /// What this device is called among the person's (device_asks).
    std::string device_name;
    uint16_t port = 0;
    std::string bind_address = {};
    std::string protocol = RATCAST_PROTOCOL;
    bool watch_network = true;
    /// Met through the DHT as well, under the same keys: no server needed.
    bool dht = true;
    /// The DHT's nodes to start from; none: the internet's — the router then
    /// asked to forward a port, and STUN where the node is seen. Given: a DHT
    /// of one's own (tests).
    std::vector<librats::HostEndpoint> dht_bootstrap = {};
    /// How often the DHT is searched for those sought: whoever an outgoing
    /// request is to, the device a link opened here is of, the person's own.
    std::chrono::milliseconds dht_search_interval = std::chrono::seconds(30);
    /// How long a device link lasts.
    std::chrono::seconds device_link_for = std::chrono::minutes(10);
};

struct PeopleHandlers {
    /// Friends or requests changed — a friend came online or went, too —
    /// or the name, or the person (a device joined as another's).
    std::function<void()> changed = {};
    /// A device asks to become this person, by the device link: answer with
    /// allow_device().
    std::function<void(const std::string& device_name)> device_asks = {};
    /// A device link opened here (join_as_device()) was answered: this device
    /// is the person now, or was refused.
    std::function<void(bool joined)> joined = {};
};

class People {
public:
    /// Start the home node. Fails with Unavailable when it cannot start, or
    /// with the person's key's own failure to load (one broken is never
    /// replaced).
    static Result<std::unique_ptr<People>> start(PeopleConfig config, PeopleHandlers handlers = {});
    ~People();
    People(const People&) = delete;
    People& operator=(const People&) = delete;

    // ── Who ─────────────────────────────────────────────────────────────────

    PersonId person() const;
    /// The person's key, for a call to certify its node with.
    PersonKey key() const;
    /// What a node of this person's tells those it meets: certified as the
    /// person's, with their room secret.
    IdentityCard card(const PeerId& device) const;
    std::string name() const;
    void set_name(std::string name);
    /// The home node's peer id.
    PeerId home() const;

    // ── Friends ─────────────────────────────────────────────────────────────

    /// The person's link, the website's.
    std::string link() const;
    /// A new secret in the link: the old one refused from now on.
    void reset_link();

    /// Ask to be a friend: by a link (parse_person_link(), a friend's), or
    /// by a room member's card. InvalidArgument for a link that is none, or
    /// one's own.
    Result<void> add(std::string_view link);
    Result<void> add(const IdentityCard& card, std::string name);
    /// Answer an incoming request; or take back an outgoing one.
    void accept(const PersonId& person);
    void decline(const PersonId& person);
    /// No longer a friend (they are not told).
    void remove(const PersonId& person);

    std::vector<Friend> friends() const;
    std::vector<FriendRequest> requests() const;
    bool is_friend(const PersonId& person) const;

    /// This device is in a room, or no longer: friends see so while it is.
    void set_in_room(bool in_room);

    // ── Devices ─────────────────────────────────────────────────────────────

    /// A device link, the website's, for device_link_for: another device that
    /// opens it asks to become this person.
    std::string device_link();
    void cancel_device_link();
    /// Answer the device that asked (device_asks).
    void allow_device(bool allowed);
    /// Become the person whose device link this is: asked of their device,
    /// answered by PeopleHandlers::joined. InvalidArgument for a link that is
    /// no device link, or one's own.
    Result<void> join_as_device(std::string_view link);

    /// The person's key as a backup's text (PersonKey::to_text()), and back:
    /// the key read in place of this one — what is known kept, the person a
    /// new one to friends. InvalidArgument for text that is no key.
    std::string backup() const;
    Result<void> restore(std::string_view backup);

private:
    struct State;
    explicit People(PeopleConfig config, PeopleHandlers handlers);

    std::unique_ptr<State> state_;
};

}  // namespace ratcast
