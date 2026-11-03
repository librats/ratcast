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
    Device(Meeting meeting, std::string name, std::filesystem::path dir)
        : meeting_(std::move(meeting)), name_(std::move(name)), dir_(std::move(dir)) {
        start();
    }

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
                            .dht_search_interval = 500ms};
        PeopleHandlers handlers{
            .changed = [this] { ++changes; },
            .device_asks =
                [this](const std::string& device) {
                    std::lock_guard lock(mutex_);
                    asking_ = device;
                },
            .joined = [this](bool joined) { joined_ = joined ? 1 : 0; },
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
    std::optional<Friend> friend_of(const People& other) const {
        const PersonId person = other.person();
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
    mutable std::mutex mutex_;
    std::optional<std::string> asking_;
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
    std::unique_ptr<Device> device(const std::string& name, const std::string& dir = {}) {
        return std::make_unique<Device>(Meeting{.servers = {server_.address()}}, name,
                                        folder_.of(name, dir));
    }

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
