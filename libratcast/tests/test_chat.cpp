// What a message may say, and what one member keeps of what was said: in the
// order it was said, each once, the last few.

#include "ratcast/session/chat.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

constexpr WallTime kNoon{std::chrono::hours(482'000)};

PeerId peer(uint8_t seed) {
    std::array<uint8_t, 32> key{};
    key.fill(seed);
    return PeerId::from_public_key(key.data(), key.size());
}

ChatMessage said(const std::string& text, WallTime sent, uint8_t by = 1) {
    return {peer(by), "member", sent, text};
}

bool valid_text(std::string text) { return is_valid(said(text, kNoon)); }

std::vector<std::string> texts(const ChatLog& log) {
    std::vector<std::string> out;
    for (const ChatMessage& m : log.messages()) out.push_back(m.text);
    return out;
}

}  // namespace

TEST(ChatMessage, ATextIsAnyTextOfAnyScriptOnAnyLines) {
    EXPECT_TRUE(valid_text("hi"));
    EXPECT_TRUE(valid_text("Привет, 李明 🎮"));
    EXPECT_TRUE(valid_text("one\ntwo\tcolumns"));
    EXPECT_TRUE(valid_text(std::string(ChatMessage::kMaxTextSize, 'x')));
    // 1000 characters of four bytes each fill it.
    std::string emoji;
    for (int i = 0; i < 1000; ++i) emoji += "🎮";
    EXPECT_TRUE(valid_text(emoji));
}

TEST(ChatMessage, ATextIsSomethingNotTooLongOfValidUtf8) {
    EXPECT_FALSE(valid_text(""));
    EXPECT_FALSE(valid_text(std::string(ChatMessage::kMaxTextSize + 1, 'x')));
    EXPECT_FALSE(valid_text("a\xD0"));         // cut short
    EXPECT_FALSE(valid_text("\xED\xA0\x80"));  // a surrogate
}

TEST(ChatMessage, ATextHasNoControlCharactersButLinesAndTabs) {
    EXPECT_FALSE(valid_text("a\rb"));
    EXPECT_FALSE(valid_text("\x1B[31mred"));
    EXPECT_FALSE(valid_text("a\xC2\x85"));  // U+0085, a C1 control
    EXPECT_FALSE(valid_text(std::string("a\0b", 3)));
}

TEST(ChatMessage, ItsNameIsAPresencesAndItWasSaidAfterTheEpoch) {
    ChatMessage m = said("hi", kNoon);
    m.name = "a\nb";
    EXPECT_FALSE(is_valid(m));
    m.name = "";
    EXPECT_TRUE(is_valid(m));
    m.sent = WallTime{-1us};
    EXPECT_FALSE(is_valid(m));
}

TEST(ChatLog, KeepsWhatWasSaidInTheOrderItWasSaid) {
    ChatLog log;
    EXPECT_TRUE(log.add(said("second", kNoon + 1s)));
    EXPECT_TRUE(log.add(said("first", kNoon)));
    EXPECT_TRUE(log.add(said("third", kNoon + 2s)));
    EXPECT_EQ(texts(log), (std::vector<std::string>{"first", "second", "third"}));
}

TEST(ChatLog, ListsWhatTwoSaidAtOnceAlikeWhicheverCameFirst) {
    ChatLog one;
    ChatLog other;
    const ChatMessage a = said("from 1", kNoon, 1);
    const ChatMessage b = said("from 2", kNoon, 2);
    ASSERT_TRUE(one.add(a) && one.add(b));
    ASSERT_TRUE(other.add(b) && other.add(a));
    EXPECT_EQ(one.messages(), other.messages());
}

TEST(ChatLog, KeepsEachOnceHoweverOftenItIsTold) {
    ChatLog log;
    EXPECT_TRUE(log.add(said("hi", kNoon)));
    EXPECT_FALSE(log.add(said("hi", kNoon)));
    EXPECT_FALSE(log.add(said("forged", kNoon))) << "known by who and when: the first stays";
    EXPECT_TRUE(log.add(said("hi", kNoon, 2))) << "another's";
    EXPECT_TRUE(log.add(said("hi", kNoon + 1us)));
    EXPECT_EQ(texts(log), (std::vector<std::string>{"hi", "hi", "hi"}));
}

TEST(ChatLog, KeepsTheLastAndLetsTheOldestGo) {
    ChatLog log(2);
    EXPECT_TRUE(log.add(said("1", kNoon + 1s)));
    EXPECT_TRUE(log.add(said("2", kNoon + 2s)));
    EXPECT_TRUE(log.add(said("3", kNoon + 3s)));
    EXPECT_EQ(texts(log), (std::vector<std::string>{"2", "3"}));
    EXPECT_FALSE(log.add(said("0", kNoon))) << "older than all it keeps: nothing new";
    EXPECT_TRUE(log.add(said("2.5", kNoon + 2500ms)));
    EXPECT_EQ(texts(log), (std::vector<std::string>{"2.5", "3"}));
}
