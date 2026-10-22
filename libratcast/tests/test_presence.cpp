// What a presence may say: a name of valid UTF-8, without control characters,
// not too long.

#include "ratcast/session/presence.h"

#include <gtest/gtest.h>

using namespace ratcast;

namespace {

bool valid_name(std::string name) { return is_valid(Presence{std::move(name)}); }

}  // namespace

TEST(Presence, ANameIsAnyTextOfAnyScript) {
    EXPECT_TRUE(valid_name(""));
    EXPECT_TRUE(valid_name("Bob"));
    EXPECT_TRUE(valid_name("Анна-Мария"));
    EXPECT_TRUE(valid_name("李明 🎮"));
    EXPECT_TRUE(valid_name(std::string(Presence::kMaxNameSize, 'x')));
    // 32 characters of four bytes each fill it.
    std::string emoji;
    for (int i = 0; i < 32; ++i) emoji += "🎮";
    EXPECT_TRUE(valid_name(emoji));
}

TEST(Presence, ANameIsNotTooLong) {
    EXPECT_FALSE(valid_name(std::string(Presence::kMaxNameSize + 1, 'x')));
}

TEST(Presence, ANameIsValidUtf8) {
    EXPECT_FALSE(valid_name("\xFF"));              // no lead byte
    EXPECT_FALSE(valid_name("a\xD0"));             // cut short
    EXPECT_FALSE(valid_name("\xD0\x41"));          // not a continuation
    EXPECT_FALSE(valid_name("\xC0\xAF"));          // overlong '/'
    EXPECT_FALSE(valid_name("\xED\xA0\x80"));      // a surrogate
    EXPECT_FALSE(valid_name("\xF4\x90\x80\x80"));  // past U+10FFFF
}

TEST(Presence, ANameHasNoControlCharacters) {
    EXPECT_FALSE(valid_name("a\nb"));
    EXPECT_FALSE(valid_name("a\x7F"));
    EXPECT_FALSE(valid_name("a\xC2\x85"));  // U+0085, a C1 control
    EXPECT_FALSE(valid_name(std::string("a\0b", 3)));
}
