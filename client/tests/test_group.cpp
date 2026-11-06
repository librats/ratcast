// A group's link, its rooms' keys, and its log: changes signed by its key,
// kept in one order everywhere, those of a type not known kept as they are.

#include "ratcast/client/group.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace ratcast;

namespace {

std::vector<uint8_t> text(std::string_view s) { return {s.begin(), s.end()}; }

GroupLink a_link() {
    const PersonKey key = PersonKey::generate();
    return GroupLink{.group = GroupId::of(key), .secret = random_bytes<16>()};
}

}  // namespace

TEST(GroupLinkTest, GoesAboutAsTheWebsitesAndAsRatsAndComesBack) {
    const GroupLink link = a_link();
    const std::string web = to_web_link(link);
    EXPECT_TRUE(web.starts_with("https://ratcast.app/g/#")) << web;
    EXPECT_EQ(parse_group_link(web), link);
    EXPECT_TRUE(to_string(link).starts_with("rats://g/"));
    EXPECT_EQ(parse_group_link(to_string(link)), link);
    // As messengers pass it on.
    std::string escaped = web;
    escaped.replace(escaped.find('.', 20), 1, "%2E");
    EXPECT_EQ(parse_group_link("  " + escaped + "/"), link);
    EXPECT_EQ(parse_group_link("https://www.ratcast.app/g/#" + web.substr(23)), link);
}

TEST(GroupLinkTest, NoOtherLinkIsAGroups) {
    const GroupLink link = a_link();
    const std::string web = to_web_link(link);
    EXPECT_FALSE(parse_group_link("https://ratcast.app/u/#" + web.substr(23)));
    EXPECT_FALSE(parse_group_link("https://ratcast.app/r/#" + web.substr(23)));
    EXPECT_FALSE(parse_group_link(web.substr(0, web.size() - 2)));
    EXPECT_FALSE(parse_group_link("rats://g/nothing"));
    EXPECT_FALSE(parse_group_link(""));
}

TEST(GroupLinkTest, EachChannelIsARoomOfItsOwnOfTheSecret) {
    const GroupLink link = a_link();
    GroupLink reset = link;
    reset.secret = random_bytes<16>();
    EXPECT_EQ(channel(link), channel(link, kMainChannel));
    EXPECT_EQ(channel(link), channel(link)) << "the same for every member";
    EXPECT_NE(channel(link), channel(link, "games"));
    EXPECT_NE(channel(link), channel(reset));
    // Of the secret: the id alone works out neither the room nor its key.
    GroupLink none = link;
    none.secret = {};
    EXPECT_NE(channel(link), channel(none));
    EXPECT_EQ(room_key(channel(link)).find(link.group.to_hex()), std::string::npos);
}

TEST(GroupLogTest, TheLastNameIsTheGroups) {
    const PersonKey key = PersonKey::generate();
    GroupLog log;
    EXPECT_TRUE(log.empty());
    EXPECT_EQ(log.name(), "");
    const GroupChange first = log.append(key, ChangeType::Named, text("Friday films"));
    EXPECT_EQ(first.seq, 1u);
    log.append(key, ChangeType::Named, text("Films"));
    EXPECT_EQ(log.last(), 2u);
    EXPECT_EQ(log.name(), "Films");
}

TEST(GroupLogTest, OnlyWhatTheGroupsKeySignedIsTaken) {
    const PersonKey key = PersonKey::generate();
    const GroupId group = GroupId::of(key);
    GroupLog theirs;
    const GroupChange named = theirs.append(key, ChangeType::Named, text("Ours"));

    GroupLog mine;
    // Another's key, or a change altered after it was signed.
    GroupLog forged;
    forged.append(PersonKey::generate(), ChangeType::Named, text("Theirs now"));
    EXPECT_FALSE(mine.merge(group, forged));
    GroupChange altered = named;
    altered.data = text("Theirs now");
    EXPECT_FALSE(mine.add(group, altered));
    EXPECT_TRUE(mine.empty());

    EXPECT_TRUE(mine.add(group, named));
    EXPECT_FALSE(mine.add(group, named)) << "taken twice";
    EXPECT_EQ(mine.name(), "Ours");
}

TEST(GroupLogTest, ChangesComeInOneOrderWhereverTheyCameFrom) {
    const PersonKey key = PersonKey::generate();
    const GroupId group = GroupId::of(key);
    GroupLog base;
    base.append(key, ChangeType::Named, text("One"));
    // Two of the maker's devices rename at once: one seq, two changes.
    GroupLog here = base;
    GroupLog there = base;
    here.append(key, ChangeType::Named, text("Here"));
    there.append(key, ChangeType::Named, text("There"));

    GroupLog a = here;
    GroupLog b = there;
    EXPECT_TRUE(a.merge(group, there));
    EXPECT_TRUE(b.merge(group, here));
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.changes().size(), 3u);
    EXPECT_EQ(a.name(), b.name());
    EXPECT_FALSE(a.merge(group, b)) << "nothing new";
}

TEST(GroupLogTest, AResetNamesTheNewSecretByItsMarkAndTheMembersItKeeps) {
    const PersonKey key = PersonKey::generate();
    const GroupId group = GroupId::of(key);
    const GroupLink old{group, random_bytes<16>()};
    GroupLog log;
    log.append(key, ChangeType::Named, text("Club"));
    EXPECT_FALSE(log.reset());
    EXPECT_TRUE(log.current(old)) << "never reset: any secret";

    const GroupLink next{group, random_bytes<16>()};
    const GroupReset reset{.mark = secret_mark(next),
                           .kept = {PersonKey::generate().id(), key.id()}};
    log.append(key, ChangeType::Reset, encode(reset));
    EXPECT_EQ(log.reset(), reset);
    EXPECT_TRUE(log.current(next));
    EXPECT_FALSE(log.current(old));
    EXPECT_EQ(log.name(), "Club") << "named as it was";
    // The mark is the secret's and the group's, and tells neither.
    EXPECT_NE(secret_mark(next), secret_mark(old));
    EXPECT_NE(secret_mark(next),
              secret_mark(GroupLink{GroupId::of(PersonKey::generate()), next.secret}));
    const std::string mark = to_hex(secret_mark(next));
    EXPECT_EQ(mark.find(to_hex(next.secret)), std::string::npos);

    // The last reset stands, and goes about with the log.
    const GroupLink last{group, random_bytes<16>()};
    log.append(key, ChangeType::Reset, encode(GroupReset{.mark = secret_mark(last)}));
    EXPECT_TRUE(log.current(last));
    EXPECT_FALSE(log.current(next));
    const std::optional<GroupLog> passed = decode_group_log(group, encode(log));
    ASSERT_TRUE(passed);
    EXPECT_TRUE(passed->current(last));

    // Data that is no reset.
    std::vector<uint8_t> bytes = encode(reset);
    EXPECT_EQ(decode_group_reset(bytes), reset);
    bytes.push_back(0);
    EXPECT_FALSE(decode_group_reset(bytes));
    EXPECT_FALSE(decode_group_reset(text("Club")));
}

TEST(GroupLogTest, AChangeOfATypeNotKnownIsKeptAndPassedOn) {
    const PersonKey key = PersonKey::generate();
    const GroupId group = GroupId::of(key);
    GroupLog later;
    later.append(key, ChangeType::Named, text("Club"));
    // A later app's channel, say.
    const GroupChange channel =
        later.append(key, static_cast<ChangeType>(77), text("voice:games"), /*version=*/3);

    const std::optional<GroupLog> here = decode_group_log(group, encode(later));
    ASSERT_TRUE(here);
    EXPECT_EQ(*here, later);
    EXPECT_EQ(here->name(), "Club");
    EXPECT_EQ(here->changes().back(), channel);
    // Passed on as it came.
    EXPECT_EQ(decode_group_log(group, encode(*here)), later);
}

TEST(GroupLogTest, BytesThatAreNoLogOfTheGroupsAreRefused) {
    const PersonKey key = PersonKey::generate();
    GroupLog log;
    log.append(key, ChangeType::Named, text("Club"));
    const std::vector<uint8_t> bytes = encode(log);

    EXPECT_FALSE(decode_group_log(GroupId::of(PersonKey::generate()), bytes)) << "another's";
    std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 1);
    EXPECT_FALSE(decode_group_log(GroupId::of(key), cut));
    std::vector<uint8_t> more = bytes;
    more.push_back(0);
    EXPECT_FALSE(decode_group_log(GroupId::of(key), more));
    EXPECT_FALSE(decode_group_log(GroupId::of(key), {}));
    const std::optional<GroupLog> none = decode_group_log(GroupId::of(key), encode(GroupLog{}));
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->empty());
}
