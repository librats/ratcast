// People on loopback: friends asked by link and from a room, accepted and
// declined, a link reset, a device that becomes its person, what is known
// kept from one run to the next — each side's home node met at a stand-in
// server, as the app's are at a ratcast-server; and with no server, through a
// DHT of the tests' own.

#include "ratcast/client/people.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "librats/node/node.h"
#include "librats/subsystems/dht_discovery.h"
#include "librats/subsystems/hole_punch.h"
#include "librats/subsystems/relay.h"
#include "librats/subsystems/rendezvous.h"

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

std::string protocol() {
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    return std::string("ratcast-test/") + test->test_suite_name() + "." + test->name();
}

class Server {
public:
    Server() : node_(node_config()) {
        librats::Rendezvous::Config meeting;
        meeting.serve = true;
        node_.add_subsystem(std::make_unique<librats::Rendezvous>(meeting));
        node_.add_subsystem(std::make_unique<librats::HolePunch>());
        librats::Relay::Config relay;
        relay.serve = true;
        node_.add_subsystem(std::make_unique<librats::Relay>(relay));
        EXPECT_TRUE(node_.start());
    }
    ~Server() { node_.stop(); }
    librats::HostEndpoint address() const { return {"127.0.0.1", node_.listen_port()}; }

private:
    static librats::NodeConfig node_config() {
        librats::NodeConfig node;
        node.bind_address = "127.0.0.1";
        node.protocol = protocol();
        return node;
    }
    librats::Node node_;
};

/// The first node of a DHT of the tests' own, which keeps what is announced
/// as the internet's many do.
class Seed {
public:
    explicit Seed(const std::filesystem::path& dir) : node_(node_config()) {
        librats::DhtDiscovery::Config config;
        config.bind_address = "127.0.0.1";
        config.data_dir = dir.string();
        // Nothing answers there: not the internet's.
        config.bootstrap_nodes = {librats::HostEndpoint("127.0.0.1", 9)};
        config.discover_external_ip = false;
        auto dht = std::make_unique<librats::DhtDiscovery>(config);
        dht->set_keys(std::vector<std::string>{});
        dht_ = node_.add_subsystem(std::move(dht));
        EXPECT_TRUE(node_.start());
        EXPECT_TRUE(wait_for([&] { return dht_->dht_port() != 0; }, 5s));
    }
    ~Seed() { node_.stop(); }
    librats::HostEndpoint address() const { return {"127.0.0.1", dht_->dht_port()}; }

private:
    static librats::NodeConfig node_config() {
        librats::NodeConfig node;
        node.bind_address = "127.0.0.1";
        node.protocol = protocol();
        return node;
    }
    librats::Node node_;
    librats::DhtDiscovery* dht_ = nullptr;
};

/// Where home nodes meet: at servers, or through a DHT (none: not so).
struct Meeting {
    std::vector<librats::HostEndpoint> servers;
    std::vector<librats::HostEndpoint> dht;
};

/// A person's device: its People, its data directory, what it was told.
class Device {
public:
    Device(Meeting meeting, std::string name, std::filesystem::path dir,
           std::chrono::milliseconds ring_for = 45s)
        : meeting_(std::move(meeting)),
          name_(std::move(name)),
          dir_(std::move(dir)),
          ring_for_(ring_for) {
        start();
    }
    // People first: its handlers write into what is below, from its threads.
    ~Device() { stop(); }

    void start() {
        PeopleConfig config{.data_dir = dir_.string(),
                            .servers = meeting_.servers,
                            .built_in_servers = {},
                            .name = name_,
                            .device_name = name_ + "'s device",
                            .bind_address = "127.0.0.1",
                            .protocol = protocol(),
                            .watch_network = false,
                            .dht = !meeting_.dht.empty(),
                            .dht_bootstrap = meeting_.dht,
                            .dht_search_interval = 500ms,
                            .ring_for = ring_for_};
        PeopleHandlers handlers{
            .changed = [this] { ++changes; },
            .device_asks =
                [this](const std::string& device) {
                    std::lock_guard lock(mutex_);
                    asking_ = device;
                },
            .joined = [this](bool joined) { joined_ = joined ? 1 : 0; },
            .rung =
                [this](const Ring& ring) {
                    std::lock_guard lock(mutex_);
                    rung_.push_back(ring);
                },
            .ring_ended =
                [this](const Ring& ring, RingEnd end) {
                    std::lock_guard lock(mutex_);
                    ended_.emplace_back(ring, end);
                },
            .shares =
                [this](const Group& group, const Group::Here& who) {
                    std::lock_guard lock(mutex_);
                    shares_.emplace_back(group.name, who.name);
                },
        };
        Result<std::unique_ptr<People>> started = People::start(config, handlers);
        ASSERT_TRUE(started);
        people = std::move(*started);
    }
    void stop() { people.reset(); }

    std::optional<std::string> asking() const {
        std::lock_guard lock(mutex_);
        return asking_;
    }
    /// The rings it was told of, and how each that ended did.
    std::vector<Ring> rung() const {
        std::lock_guard lock(mutex_);
        return rung_;
    }
    std::optional<RingEnd> ended(const Ring& ring) const {
        std::lock_guard lock(mutex_);
        for (const auto& [r, end] : ended_)
            if (r.id == ring.id) return end;
        return std::nullopt;
    }
    /// Who it was told shares, in which group: by name.
    std::vector<std::pair<std::string, std::string>> shares() const {
        std::lock_guard lock(mutex_);
        return shares_;
    }

    bool has_friend(const People& other) const {
        const PersonId person = other.person();
        for (const Friend& f : people->friends())
            if (f.person == person) return true;
        return false;
    }
    std::optional<FriendRequest> request_of(const People& other) const {
        const PersonId person = other.person();
        for (const FriendRequest& r : people->requests())
            if (r.person == person) return r;
        return std::nullopt;
    }
    std::optional<Friend> friend_of(const People& other) const { return friend_of(other.person()); }
    std::optional<Friend> friend_of(const PersonId& person) const {
        for (const Friend& f : people->friends())
            if (f.person == person) return f;
        return std::nullopt;
    }

    std::unique_ptr<People> people;
    std::atomic<int> changes{0};
    std::atomic<int> joined_{-1};

private:
    Meeting meeting_;
    std::string name_;
    std::filesystem::path dir_;
    std::chrono::milliseconds ring_for_;
    mutable std::mutex mutex_;
    std::optional<std::string> asking_;
    std::vector<Ring> rung_;
    std::vector<std::pair<Ring, RingEnd>> ended_;
    std::vector<std::pair<std::string, std::string>> shares_;
};

/// Each test's devices in a folder of their own.
class Folder {
public:
    Folder()
        : root_(std::filesystem::temp_directory_path() /
                ("ratcast-people-" + to_hex(random_bytes<8>()))) {}
    ~Folder() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    std::filesystem::path of(const std::string& name, const std::string& dir) const {
        return root_ / (dir.empty() ? name : dir);
    }

private:
    std::filesystem::path root_;
};

class PeopleTest : public ::testing::Test {
protected:
    std::unique_ptr<Device> device(const std::string& name, const std::string& dir = {},
                                   std::chrono::milliseconds ring_for = 45s) {
        return std::make_unique<Device>(Meeting{.servers = {server_.address()}}, name,
                                        folder_.of(name, dir), ring_for);
    }

    /// Ann and Bob friends, each seen online by the other.
    void befriend(Device& ann, Device& bob) {
        ASSERT_TRUE(bob.people->add(ann.people->link()));
        ASSERT_TRUE(wait_for([&] { return ann.request_of(*bob.people).has_value(); }));
        ann.people->accept(bob.people->person());
        ASSERT_TRUE(wait_for([&] {
            const auto a = ann.friend_of(*bob.people);
            const auto b = bob.friend_of(*ann.people);
            return a && b && a->online && b->online;
        }));
    }
    /// A second device of `person`'s, by a device link.
    std::unique_ptr<Device> second_device(Device& person, const std::string& name) {
        auto device = this->device(name, name);
        EXPECT_TRUE(device->people->join_as_device(person.people->device_link()));
        EXPECT_TRUE(wait_for([&] { return person.asking().has_value(); }));
        person.people->allow_device(true);
        EXPECT_TRUE(wait_for([&] { return device->joined_.load() == 1; }));
        return device;
    }

    librats::HostEndpoint server_address() const { return server_.address(); }

    Folder folder_;
    Server server_;
};

/// No server: home nodes met through the DHT alone.
class PeopleOverTheDhtTest : public ::testing::Test {
protected:
    std::unique_ptr<Device> device(const std::string& name, const std::string& dir = {}) {
        return std::make_unique<Device>(Meeting{.dht = {seed_.address()}}, name,
                                        folder_.of(name, dir));
    }

    Folder folder_;
    Seed seed_{folder_.of("seed", {})};
};

TEST(PersonLinks, AreWrittenAndReadBothWays) {
    const PersonLink friendly{.kind = PersonLink::Kind::Friend,
                              .person = PersonKey::generate().id(),
                              .secret = random_bytes<16>()};
    const std::string web = to_web_link(friendly);
    EXPECT_TRUE(web.starts_with("https://ratcast.app/u/#")) << web;
    EXPECT_EQ(parse_person_link(web), friendly);
    EXPECT_EQ(parse_person_link(to_string(friendly)), friendly);
    EXPECT_TRUE(to_string(friendly).starts_with("rats://u/"));

    PersonLink device = friendly;
    device.kind = PersonLink::Kind::Device;
    EXPECT_TRUE(to_web_link(device).starts_with("https://ratcast.app/d/#"));
    EXPECT_EQ(parse_person_link(to_web_link(device)), device);

    // As messengers hand them on: the dot escaped, through www., a slash after.
    std::string escaped = web;
    escaped.replace(escaped.find('.', 20), 1, "%2E");
    EXPECT_EQ(parse_person_link(escaped), friendly) << escaped;
    std::string www = web;
    www.replace(0, 8, "https://www.");
    EXPECT_EQ(parse_person_link(www), friendly) << www;
    EXPECT_EQ(parse_person_link(web + "/"), friendly);

    for (const std::string bad :
         {std::string("https://ratcast.app/r/#1.2.3.4:5"), web.substr(0, web.size() - 1),
          std::string("rats://u/"), std::string("https://ratcast.app/u/#a.b"),
          web.substr(0, web.find('.', 20))}) {
        EXPECT_FALSE(parse_person_link(bad)) << bad;
    }
}

TEST_F(PeopleTest, OneAsksByLinkTheOtherAcceptsAndBothAreFriends) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    ASSERT_TRUE(bob->people->add(ann->people->link()));
    ASSERT_EQ(bob->people->requests().size(), 1u);
    EXPECT_FALSE(bob->people->requests()[0].incoming);

    ASSERT_TRUE(wait_for([&] { return ann->request_of(*bob->people).has_value(); }));
    const FriendRequest asked = *ann->request_of(*bob->people);
    EXPECT_TRUE(asked.incoming);
    EXPECT_EQ(asked.name, "Bob");
    EXPECT_EQ(bob->request_of(*ann->people)->name, "Ann") << "met, its name known";

    ann->people->accept(bob->people->person());
    ASSERT_TRUE(wait_for([&] { return bob->has_friend(*ann->people); }));
    EXPECT_TRUE(ann->has_friend(*bob->people));
    EXPECT_TRUE(ann->people->requests().empty());
    EXPECT_TRUE(bob->people->requests().empty());
    EXPECT_EQ(ann->friend_of(*bob->people)->shared, bob->friend_of(*ann->people)->shared)
        << "a secret the two share";
    EXPECT_EQ(bob->friend_of(*ann->people)->name, "Ann");

    // Kept for the next run.
    bob->stop();
    bob->start();
    EXPECT_TRUE(bob->has_friend(*ann->people));
}

TEST_F(PeopleTest, ARequestWaitsForThePersonToComeOnline) {
    auto ann = device("Ann");
    const std::string link = ann->people->link();
    const PersonId ann_id = ann->people->person();
    ann->stop();
    auto bob = device("Bob");
    ASSERT_TRUE(bob->people->add(link));
    std::this_thread::sleep_for(300ms);
    ann->start();
    ASSERT_EQ(ann->people->person(), ann_id);
    EXPECT_TRUE(wait_for([&] { return ann->request_of(*bob->people).has_value(); }));
}

TEST_F(PeopleTest, ADeclinedRequestIsToldAndAResetLinkRefused) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    auto carl = device("Carl");
    const std::string old_link = ann->people->link();
    ASSERT_TRUE(bob->people->add(old_link));
    ASSERT_TRUE(wait_for([&] { return ann->request_of(*bob->people).has_value(); }));
    ann->people->decline(bob->people->person());
    EXPECT_TRUE(wait_for([&] {
        const auto r = bob->request_of(*ann->people);
        return r && r->declined;
    }));
    EXPECT_FALSE(ann->request_of(*bob->people)) << "declined, not listed";
    EXPECT_FALSE(bob->has_friend(*ann->people));

    ann->people->reset_link();
    EXPECT_NE(ann->people->link(), old_link);
    ASSERT_TRUE(carl->people->add(old_link));
    std::this_thread::sleep_for(1500ms);
    EXPECT_FALSE(ann->request_of(*carl->people)) << "asked by a link reset";
    ASSERT_TRUE(carl->people->add(ann->people->link()));
    EXPECT_TRUE(wait_for([&] { return ann->request_of(*carl->people).has_value(); }));
}

TEST_F(PeopleTest, AFriendIsAskedFromARoomByTheirCard) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    // What Ann's node in a room tells Bob's.
    const IdentityCard card = ann->people->card(ann->people->home());
    ASSERT_TRUE(bob->people->add(card, "Ann"));
    EXPECT_EQ(bob->request_of(*ann->people)->name, "Ann");
    ASSERT_TRUE(wait_for([&] { return ann->request_of(*bob->people).has_value(); }));
    ann->people->accept(bob->people->person());
    EXPECT_TRUE(wait_for([&] { return bob->has_friend(*ann->people); }));

    IdentityCard none = card;
    none.request_secret = {};
    EXPECT_FALSE(bob->people->add(none, "Ann")) << "no secret to ask with";
    EXPECT_FALSE(bob->people->add(bob->people->card(bob->people->home()), "Bob")) << "oneself";
}

TEST_F(PeopleTest, TwoWhoAskEachOtherAreFriendsWithOneSecret) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    ASSERT_TRUE(ann->people->add(bob->people->link()));
    ASSERT_TRUE(bob->people->add(ann->people->link()));
    ASSERT_TRUE(wait_for([&] {
        return ann->has_friend(*bob->people) && bob->has_friend(*ann->people) &&
               ann->friend_of(*bob->people)->shared == bob->friend_of(*ann->people)->shared;
    }));
}

TEST_F(PeopleTest, ADeviceBecomesItsPersonAndTheirDevicesKeepAlike) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    ASSERT_TRUE(bob->people->add(ann->people->link()));
    ASSERT_TRUE(wait_for([&] { return ann->request_of(*bob->people).has_value(); }));
    ann->people->accept(bob->people->person());
    ASSERT_TRUE(wait_for([&] { return bob->has_friend(*ann->people); }));

    auto phone = device("Phone", "Ann's phone");
    ASSERT_NE(phone->people->person(), ann->people->person());
    ASSERT_TRUE(phone->people->join_as_device(ann->people->device_link()));
    ASSERT_TRUE(wait_for([&] { return ann->asking().has_value(); }));
    EXPECT_EQ(*ann->asking(), "Phone's device");
    ann->people->allow_device(true);
    ASSERT_TRUE(wait_for([&] { return phone->joined_.load() == 1; }));
    EXPECT_EQ(phone->people->person(), ann->people->person());
    EXPECT_EQ(phone->people->name(), "Ann");
    EXPECT_TRUE(phone->has_friend(*bob->people));

    // What one of them does, the other comes to know.
    phone->people->remove(bob->people->person());
    EXPECT_TRUE(wait_for([&] { return !ann->has_friend(*bob->people); }));
    ann->people->set_name("Anna");
    EXPECT_TRUE(wait_for([&] { return phone->people->name() == "Anna"; }));

    // The phone is Ann the next run too.
    const PersonId ann_id = ann->people->person();
    phone->stop();
    phone->start();
    EXPECT_EQ(phone->people->person(), ann_id);
}

// Friends see each other online, and in a room, at the server — not met: a
// friend's device that comes back meets nobody, and is seen all the same.
TEST_F(PeopleTest, FriendsSeeEachOtherOnlineAndInARoom) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    ASSERT_TRUE(bob->people->add(ann->people->link()));
    ASSERT_TRUE(wait_for([&] { return ann->request_of(*bob->people).has_value(); }));
    ann->people->accept(bob->people->person());
    ASSERT_TRUE(wait_for([&] { return bob->has_friend(*ann->people); }));
    EXPECT_TRUE(wait_for([&] { return ann->friend_of(*bob->people)->online; }));
    EXPECT_TRUE(wait_for([&] { return bob->friend_of(*ann->people)->online; }));
    EXPECT_FALSE(ann->friend_of(*bob->people)->in_room);

    const PersonId bob_id = bob->people->person();
    bob->stop();
    EXPECT_TRUE(wait_for([&] { return !ann->friend_of(bob_id)->online; })) << "seen online, gone";
    bob->start();
    EXPECT_TRUE(wait_for([&] { return ann->friend_of(*bob->people)->online; }))
        << "come back, unseen";
    EXPECT_TRUE(wait_for([&] { return bob->friend_of(*ann->people)->online; }));

    const int changes = ann->changes.load();
    bob->people->set_in_room(true);
    EXPECT_TRUE(wait_for([&] { return ann->friend_of(*bob->people)->in_room; }));
    EXPECT_GT(ann->changes.load(), changes) << "not told";
    bob->people->set_in_room(false);
    EXPECT_TRUE(wait_for([&] { return !ann->friend_of(*bob->people)->in_room; }));
    EXPECT_TRUE(ann->friend_of(*bob->people)->online);

    // The person's other devices see them too: what to watch is kept alike.
    auto phone = device("Phone", "Ann's phone");
    ASSERT_TRUE(phone->people->join_as_device(ann->people->device_link()));
    ASSERT_TRUE(wait_for([&] { return ann->asking().has_value(); }));
    ann->people->allow_device(true);
    ASSERT_TRUE(wait_for([&] { return phone->joined_.load() == 1; }));
    EXPECT_TRUE(wait_for([&] {
        const auto bob_seen = phone->friend_of(*bob->people);
        return bob_seen && bob_seen->online;
    }));
}

// A friend is rung on each device of theirs online, met at the server for it;
// one answers, the caller is told, and the other stops ringing.
TEST_F(PeopleTest, AFriendIsRungOnEachDeviceAndAnsweredOnOne) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    befriend(*ann, *bob);
    auto phone = second_device(*ann, "Ann's phone");
    // Not met: the ring finds them.
    bob->stop();
    bob->start();
    ASSERT_TRUE(wait_for([&] { return bob->friend_of(*ann->people)->online; }));

    const Result<Ring> ring = bob->people->ring(ann->people->person(), "rats://room");
    ASSERT_TRUE(ring);
    EXPECT_EQ(bob->people->ringing()->id, ring->id);
    ASSERT_TRUE(wait_for([&] { return ann->rung().size() == 1 && phone->rung().size() == 1; }));
    const Ring heard = ann->rung()[0];
    EXPECT_EQ(heard.id, ring->id);
    EXPECT_EQ(heard.person, bob->people->person());
    EXPECT_EQ(heard.name, "Bob");
    EXPECT_EQ(heard.room, "rats://room");
    EXPECT_EQ(ann->people->rings().size(), 1u);

    ann->people->answer(heard.id, true);
    EXPECT_TRUE(wait_for([&] { return bob->ended(*ring) == RingEnd::Accepted; }));
    EXPECT_TRUE(wait_for([&] { return phone->ended(*ring) == RingEnd::AnsweredElsewhere; }));
    EXPECT_FALSE(bob->people->ringing());
    EXPECT_TRUE(ann->people->rings().empty());
    EXPECT_TRUE(wait_for([&] { return phone->people->rings().empty(); }));
    EXPECT_FALSE(ann->ended(*ring)) << "told of its own answer";
}

// A ring declined, unanswered or taken back is told so on both sides; only a
// friend is rung.
TEST_F(PeopleTest, ARingDeclinedMissedOrTakenBackIsToldSo) {
    auto ann = device("Ann", {}, 1s);
    auto bob = device("Bob", {}, 1s);
    befriend(*ann, *bob);
    const PersonId ann_id = ann->people->person();

    const Result<Ring> declined = bob->people->ring(ann_id, "rats://room");
    ASSERT_TRUE(declined);
    ASSERT_TRUE(wait_for([&] { return ann->rung().size() == 1; }));
    ann->people->answer(declined->id, false);
    EXPECT_TRUE(wait_for([&] { return bob->ended(*declined) == RingEnd::Declined; }));

    const Result<Ring> missed = bob->people->ring(ann_id, "rats://room");
    ASSERT_TRUE(missed);
    ASSERT_TRUE(wait_for([&] { return ann->rung().size() == 2; }));
    EXPECT_TRUE(wait_for([&] { return bob->ended(*missed) == RingEnd::Missed; }, 5s));
    EXPECT_TRUE(wait_for([&] { return ann->ended(*missed) == RingEnd::Missed; }, 5s));
    EXPECT_FALSE(bob->people->ringing());

    const Result<Ring> taken_back = bob->people->ring(ann_id, "rats://room");
    ASSERT_TRUE(taken_back);
    ASSERT_TRUE(wait_for([&] { return ann->rung().size() == 3; }));
    bob->people->hang_up();
    EXPECT_TRUE(wait_for([&] { return ann->ended(*taken_back) == RingEnd::Cancelled; }));
    EXPECT_FALSE(bob->ended(*taken_back)) << "told of its own hanging up";

    auto carl = device("Carl");
    EXPECT_FALSE(carl->people->ring(ann_id, "rats://room")) << "rang one who is no friend";
    EXPECT_FALSE(bob->people->ring(ann_id, "")) << "rang to no room";
}

TEST_F(PeopleTest, ADeviceRefusedOrWithoutTheLinkBecomesNobody) {
    auto ann = device("Ann");
    auto phone = device("Phone");
    const std::string link = ann->people->device_link();
    ASSERT_TRUE(phone->people->join_as_device(link));
    ASSERT_TRUE(wait_for([&] { return ann->asking().has_value(); }));
    ann->people->allow_device(false);
    ASSERT_TRUE(wait_for([&] { return phone->joined_.load() == 0; }));
    EXPECT_NE(phone->people->person(), ann->people->person());

    // A friend's link is no device's, and a device link taken back is nothing.
    EXPECT_FALSE(phone->people->join_as_device(ann->people->link()));
    const std::string taken_back = ann->people->device_link();
    ann->people->cancel_device_link();
    auto tablet = device("Tablet");
    ASSERT_TRUE(tablet->people->join_as_device(taken_back));
    std::this_thread::sleep_for(1500ms);
    EXPECT_EQ(tablet->joined_.load(), -1);
}

TEST_F(PeopleTest, ABackupBringsThePersonBack) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    ASSERT_TRUE(bob->people->add(ann->people->link()));
    ASSERT_TRUE(wait_for([&] { return ann->request_of(*bob->people).has_value(); }));
    ann->people->accept(bob->people->person());
    ASSERT_TRUE(wait_for([&] { return bob->has_friend(*ann->people); }));
    const std::string backup = ann->people->backup();
    const PersonId ann_id = ann->people->person();
    ann->stop();

    auto fresh = device("Someone", "Ann's new laptop");
    EXPECT_FALSE(fresh->people->restore("nonsense"));
    ASSERT_TRUE(fresh->people->restore(backup));
    EXPECT_EQ(fresh->people->person(), ann_id);
    EXPECT_TRUE(fresh->has_friend(*bob->people));
}

TEST_F(PeopleOverTheDhtTest, OneAsksByLinkTheOtherAcceptsWithNoServer) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    ASSERT_TRUE(bob->people->add(ann->people->link()));
    EXPECT_FALSE(bob->people->requests()[0].delivered) << "looked for";

    ASSERT_TRUE(wait_for([&] { return ann->request_of(*bob->people).has_value(); }, 20s));
    EXPECT_TRUE(wait_for([&] { return bob->request_of(*ann->people)->delivered; }))
        << "found, and told";
    ann->people->accept(bob->people->person());
    EXPECT_TRUE(wait_for([&] { return bob->has_friend(*ann->people); }));
    EXPECT_TRUE(ann->has_friend(*bob->people));
}

TEST_F(PeopleOverTheDhtTest, ARequestWaitsForThePersonToComeOnline) {
    auto ann = device("Ann");
    const std::string link = ann->people->link();
    ann->stop();
    auto bob = device("Bob");
    ASSERT_TRUE(bob->people->add(link));
    std::this_thread::sleep_for(1500ms);
    EXPECT_FALSE(bob->people->requests()[0].delivered);
    ann->start();
    EXPECT_TRUE(wait_for([&] { return ann->request_of(*bob->people).has_value(); }, 20s));
}

TEST_F(PeopleOverTheDhtTest, ADeviceBecomesItsPersonWithNoServer) {
    auto ann = device("Ann");
    auto phone = device("Phone", "Ann's phone");
    ASSERT_TRUE(phone->people->join_as_device(ann->people->device_link()));
    ASSERT_TRUE(wait_for([&] { return ann->asking().has_value(); }, 20s));
    ann->people->allow_device(true);
    ASSERT_TRUE(wait_for([&] { return phone->joined_.load() == 1; }));
    EXPECT_EQ(phone->people->person(), ann->people->person());

    // Its devices meet under their key, and keep alike.
    ann->people->set_name("Anna");
    EXPECT_TRUE(wait_for([&] { return phone->people->name() == "Anna"; }, 20s));
}

}  // namespace

// ── Groups ──────────────────────────────────────────────────────────────────

namespace {

/// A call in a group's room, as far as the servers see: a node registered
/// under the room's key.
class InTheRoom {
public:
    InTheRoom(const librats::HostEndpoint& server, const std::string& key) : node_(node_config()) {
        librats::Rendezvous::Config meeting;
        meeting.servers = {server};
        meeting.keys = {key};
        node_.add_subsystem(std::make_unique<librats::Rendezvous>(meeting));
        EXPECT_TRUE(node_.start());
    }
    ~InTheRoom() { node_.stop(); }

private:
    static librats::NodeConfig node_config() {
        librats::NodeConfig node;
        node.bind_address = "127.0.0.1";
        node.protocol = protocol();
        return node;
    }
    librats::Node node_;
};

std::optional<Group> group_of(const Device& device, const GroupId& id) {
    return device.people->group(id);
}

/// Who `device` sees in the group's room, by name, "*" after one sharing.
std::string here(const Device& device, const GroupId& id) {
    std::string out;
    const std::optional<Group> group = group_of(device, id);
    if (!group) return "(none)";
    for (const Group::Here& who : group->here)
        out += (out.empty() ? "" : " ") + who.name + (who.sharing ? "*" : "");
    return out;
}

}  // namespace

TEST_F(PeopleTest, AGroupIsJoinedByItsLinkAndCaughtUpByAMemberInItsRoom) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    EXPECT_FALSE(ann->people->make_group("  ")) << "a group has a name";
    const Result<Group> made = ann->people->make_group("Films");
    ASSERT_TRUE(made);
    EXPECT_EQ(made->name, "Films");
    EXPECT_TRUE(made->made);
    EXPECT_EQ(made->room, channel(*parse_group_link(made->link)));
    ASSERT_EQ(ann->people->groups().size(), 1u);

    const Result<Group> joined = bob->people->join_group(made->link);
    ASSERT_TRUE(joined);
    EXPECT_EQ(joined->id, made->id);
    EXPECT_EQ(joined->name, "") << "nobody met yet";
    EXPECT_FALSE(joined->made);
    EXPECT_EQ(joined->room, made->room) << "one room";
    EXPECT_FALSE(bob->people->join_group(ann->people->link())) << "a person's link";

    // Ann in its room: her home node met there by Bob's, who knows nothing yet.
    ann->people->set_in_room(true, made->id);
    ASSERT_TRUE(wait_for([&] { return group_of(*bob, made->id)->name == "Films"; }));

    // Renamed by its maker alone, told to the member met.
    EXPECT_FALSE(bob->people->rename_group(made->id, "Bob's"));
    ASSERT_TRUE(ann->people->rename_group(made->id, "Friday films"));
    EXPECT_TRUE(wait_for([&] { return group_of(*bob, made->id)->name == "Friday films"; }));
    EXPECT_EQ(group_of(*ann, made->id)->name, "Friday films");

    // Kept for the next run.
    bob->stop();
    bob->start();
    ASSERT_TRUE(group_of(*bob, made->id));
    EXPECT_EQ(group_of(*bob, made->id)->name, "Friday films");
}

TEST_F(PeopleTest, AGroupsSecretIsWhatLetsOneIn) {
    auto ann = device("Ann");
    auto eve = device("Eve");
    const Group made = *ann->people->make_group("Films");
    ann->people->set_in_room(true, made.id);
    // Its id, with a secret of her own.
    GroupLink guessed = *parse_group_link(made.link);
    guessed.secret = random_bytes<16>();
    const Result<Group> joined = eve->people->join_group(to_web_link(guessed));
    ASSERT_TRUE(joined);
    EXPECT_NE(joined->room, made.room) << "another room";
    std::this_thread::sleep_for(1500ms);
    EXPECT_EQ(group_of(*eve, made.id)->name, "") << "told nothing";
}

TEST_F(PeopleTest, WhoIsInAGroupsRoomIsSeenFromTheList) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    const Group made = *ann->people->make_group("Films");
    ASSERT_TRUE(bob->people->join_group(made.link));
    EXPECT_EQ(group_of(*ann, made.id)->present, 0u);

    {
        InTheRoom carl(server_address(), room_key(made.room));
        InTheRoom dan(server_address(), room_key(made.room));
        EXPECT_TRUE(wait_for([&] {
            return group_of(*ann, made.id)->present == 2 && group_of(*bob, made.id)->present == 2;
        }));
    }
    EXPECT_TRUE(wait_for([&] { return group_of(*bob, made.id)->present == 0; }));

    // Left: neither listed nor watched.
    bob->people->leave_group(made.id);
    EXPECT_FALSE(group_of(*bob, made.id));
    EXPECT_TRUE(bob->people->groups().empty());
}

TEST_F(PeopleTest, WhoIsInAGroupsRoomAndWhoSharesIsToldToTheMembersOutside) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    auto carl = device("Carl");
    const Group made = *ann->people->make_group("Films");
    ASSERT_TRUE(bob->people->join_group(made.link));
    ASSERT_TRUE(carl->people->join_group(made.link));
    const std::string key = room_key(made.room);

    // Ann in its room: her call there, her home node telling the members met.
    auto ann_call = std::make_unique<InTheRoom>(server_address(), key);
    ann->people->set_in_room(true, made.id);
    EXPECT_TRUE(wait_for([&] {
        return here(*bob, made.id) == "Ann" && here(*carl, made.id) == "Ann";
    })) << here(*bob, made.id)
        << " / " << here(*carl, made.id);
    EXPECT_EQ(group_of(*bob, made.id)->name, "Films");
    EXPECT_EQ(here(*ann, made.id), "") << "nobody but her";

    // Sharing: seen, and a notice to each outside.
    ann->people->set_sharing(true);
    EXPECT_TRUE(wait_for([&] { return bob->shares().size() == 1 && carl->shares().size() == 1; }));
    EXPECT_EQ(bob->shares().front(), (std::pair<std::string, std::string>{"Films", "Ann"}));
    EXPECT_EQ(here(*bob, made.id), "Ann*");

    // Bob comes in: Carl sees him there, and Bob, there, is told of no share.
    auto bob_call = std::make_unique<InTheRoom>(server_address(), key);
    bob->people->set_in_room(true, made.id);
    EXPECT_TRUE(wait_for([&] { return here(*carl, made.id) == "Ann* Bob"; }))
        << here(*carl, made.id);
    ann->people->set_sharing(false);
    EXPECT_TRUE(wait_for([&] { return here(*carl, made.id) == "Ann Bob"; }));
    ann->people->set_sharing(true);
    EXPECT_TRUE(wait_for([&] { return carl->shares().size() == 2; }));
    EXPECT_EQ(bob->shares().size(), 1u);

    // Dan, joining as Ann shares, finds her so: a notice too.
    auto dan = device("Dan");
    ASSERT_TRUE(dan->people->join_group(made.link));
    EXPECT_TRUE(wait_for([&] { return here(*dan, made.id) == "Ann* Bob"; })) << here(*dan, made.id);
    EXPECT_EQ(dan->shares().size(), 1u);

    // Ann leaves; then Bob, and nobody is there.
    ann->people->set_in_room(false);
    ann_call.reset();
    EXPECT_TRUE(wait_for([&] { return here(*carl, made.id) == "Bob"; })) << here(*carl, made.id);
    bob->people->set_in_room(false);
    bob_call.reset();
    EXPECT_TRUE(wait_for(
        [&] { return here(*carl, made.id).empty() && group_of(*carl, made.id)->present == 0; }));
}

TEST_F(PeopleTest, AGroupIsKeptOnEachDeviceOfThePersonChangedOnAnyAndLeftOnAll) {
    auto ann = device("Ann");
    auto phone = second_device(*ann, "Ann's phone");
    const Group made = *ann->people->make_group("Films");
    ASSERT_TRUE(wait_for([&] { return group_of(*phone, made.id).has_value(); }));
    EXPECT_TRUE(group_of(*phone, made.id)->made) << "its key there too";
    EXPECT_EQ(group_of(*phone, made.id)->link, made.link);

    ASSERT_TRUE(phone->people->rename_group(made.id, "Our films"));
    EXPECT_TRUE(wait_for([&] { return group_of(*ann, made.id)->name == "Our films"; }));

    ann->people->leave_group(made.id);
    EXPECT_TRUE(wait_for([&] { return !group_of(*phone, made.id); }));
    // Joined again by its link: a member again, its log as it was.
    ASSERT_TRUE(phone->people->join_group(made.link));
    EXPECT_EQ(group_of(*phone, made.id)->name, "Our films");
    EXPECT_TRUE(wait_for([&] { return group_of(*ann, made.id).has_value(); }));

    // Its link reset on one, the new link on both; the old opened again
    // there changes nothing.
    const Result<Group> reset = phone->people->reset_group_link(made.id);
    ASSERT_TRUE(reset);
    EXPECT_TRUE(wait_for([&] { return group_of(*ann, made.id)->link == reset->link; }));
    ASSERT_TRUE(ann->people->join_group(made.link));
    EXPECT_EQ(group_of(*ann, made.id)->link, reset->link);
}

TEST_F(PeopleTest, AGroupsLinkResetKeepsItsMembersAndShutsOutTheOldLink) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    auto carl = device("Carl");
    const Group made = *ann->people->make_group("Films");
    ASSERT_TRUE(bob->people->join_group(made.link));
    ASSERT_TRUE(carl->people->join_group(made.link));
    // Ann in its room: both met there, members from then on.
    auto ann_call = std::make_unique<InTheRoom>(server_address(), room_key(made.room));
    ann->people->set_in_room(true, made.id);
    ASSERT_TRUE(wait_for([&] {
        return group_of(*bob, made.id)->name == "Films" &&
               group_of(*carl, made.id)->name == "Films";
    }));
    ann->people->set_in_room(false);
    ann_call.reset();
    bob->stop();  // off as the link is reset

    EXPECT_FALSE(carl->people->reset_group_link(made.id)) << "its maker's alone";
    const Result<Group> reset = ann->people->reset_group_link(made.id);
    ASSERT_TRUE(reset);
    EXPECT_NE(reset->link, made.link);
    EXPECT_NE(reset->room, made.room) << "its room another";
    EXPECT_EQ(reset->name, "Films");

    // Carl, met, given the new link; Bob as he comes back, by whoever has it.
    EXPECT_TRUE(wait_for([&] { return group_of(*carl, made.id)->link == reset->link; }));
    auto eve = device("Eve");
    ASSERT_TRUE(eve->people->join_group(made.link));  // the old link, after
    bob->start();
    EXPECT_TRUE(wait_for([&] { return group_of(*bob, made.id)->link == reset->link; }));
    EXPECT_EQ(group_of(*bob, made.id)->room, reset->room);

    // Eve, with the old link alone, is not given the new one (what the old
    // secret was told, the name, she may be).
    std::this_thread::sleep_for(1500ms);
    EXPECT_EQ(group_of(*eve, made.id)->link, made.link);
    EXPECT_NE(group_of(*eve, made.id)->room, reset->room);

    // The new link lets in: Dan, met in its room.
    auto dan = device("Dan");
    ASSERT_TRUE(dan->people->join_group(reset->link));
    ann_call = std::make_unique<InTheRoom>(server_address(), room_key(reset->room));
    ann->people->set_in_room(true, made.id);
    EXPECT_TRUE(wait_for([&] { return group_of(*dan, made.id)->name == "Films"; }));

    // The old link opened by a member changes nothing; the new one is kept.
    ASSERT_TRUE(bob->people->join_group(made.link));
    EXPECT_EQ(group_of(*bob, made.id)->link, reset->link);
    bob->stop();
    bob->start();
    EXPECT_EQ(group_of(*bob, made.id)->link, reset->link);
}

TEST_F(PeopleTest, AFriendRungToAGroupsRoomIsToldWhichGroup) {
    auto ann = device("Ann");
    auto bob = device("Bob");
    befriend(*ann, *bob);
    const Group made = *ann->people->make_group("Films");
    ASSERT_TRUE(ann->people->ring(bob->people->person(), made.link));
    ASSERT_TRUE(wait_for([&] { return bob->people->rings().size() == 1; }));
    EXPECT_EQ(bob->people->rings().front().group, "Films");
    EXPECT_EQ(bob->people->rings().front().room, made.link);

    // A room of nobody's: no group.
    ASSERT_TRUE(
        ann->people->ring(bob->people->person(), to_web_link(RatsLink{RoomSecret::random()})));
    ASSERT_TRUE(wait_for([&] {
        const std::vector<Ring> rings = bob->people->rings();
        return rings.size() == 1 && rings.front().room != made.link;
    }));
    EXPECT_EQ(bob->people->rings().front().group, "");
}
