// What a file of the chat is known by, and the name it is saved under here.

#include "ratcast/client/chat_files.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace ratcast;

TEST(ChatFiles, AFilesIdIsItsBytesHashedWhereverTheyAre) {
    const std::string text = "what to bring";
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    const FileId id = file_id(bytes);
    EXPECT_NE(id, FileId{});
    EXPECT_NE(file_id(std::vector<uint8_t>(bytes.begin(), bytes.end() - 1)), id);
    EXPECT_EQ(file_id_of_hex(to_hex(id)), id);
    EXPECT_EQ(to_hex(id).size(), 64u);
    EXPECT_EQ(file_id("no such file here").error(), Errc::Unavailable);
}

TEST(ChatFiles, AnIdIsSixtyFourHexDigits) {
    EXPECT_FALSE(file_id_of_hex(""));
    EXPECT_FALSE(file_id_of_hex(std::string(63, 'a')));
    EXPECT_FALSE(file_id_of_hex(std::string(63, 'a') + "g"));
    const std::optional<FileId> upper = file_id_of_hex(std::string(64, 'F'));
    ASSERT_TRUE(upper);
    EXPECT_EQ((*upper)[0], 0xFF);
}

TEST(ChatFiles, ANameAnotherGaveIsOneAnySystemTakes) {
    EXPECT_EQ(local_file_name("отпуск 1.jpg"), "отпуск 1.jpg");
    EXPECT_EQ(local_file_name("notes.tar.gz"), "notes.tar.gz");
    // What Windows has in no name — ':' a stream of another file.
    EXPECT_EQ(local_file_name("a:b.txt"), "a_b.txt");
    EXPECT_EQ(local_file_name("what?<now>|\"*.txt"), "what__now____.txt");
    EXPECT_EQ(local_file_name("trailing. . "), "trailing");
    EXPECT_EQ(local_file_name("..."), "file");
    // Its devices, whatever follows.
    EXPECT_EQ(local_file_name("CON"), "_CON");
    EXPECT_EQ(local_file_name("nul.txt"), "_nul.txt");
    EXPECT_EQ(local_file_name("Com7.tar.gz"), "_Com7.tar.gz");
    EXPECT_EQ(local_file_name("LPT1 .log"), "_LPT1 .log");
    EXPECT_EQ(local_file_name("COM0.txt"), "COM0.txt");
    EXPECT_EQ(local_file_name("CONSOLE.txt"), "CONSOLE.txt");
    EXPECT_EQ(local_file_name("auxiliary"), "auxiliary");
}
