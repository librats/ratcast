// Putting an update in place, on folders of the test's own: a folder's files
// replaced, the old ones left aside and removed later, all put back when one
// cannot be moved; a staged update applied once, never from outside its
// staging; the folder an archive unpacked to found; a file replaced, made
// executable.

#include "install.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

using namespace ratcast::app::update;

namespace {

class InstallTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("ratcast-install-" +
                 std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    static void write(const fs::path& file, const std::string& text) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
    static std::string read(const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    static fs::path aside(fs::path file) { return file += kOldSuffix; }

    fs::path root_;
};

}  // namespace

TEST_F(InstallTest, AFoldersFilesAreReplacedTheOldOnesLeftAside) {
    const fs::path app = root_ / "app";
    const fs::path update = root_ / "update";
    write(app / "ratcast.exe", "old");
    write(app / "qml" / "plugin.dll", "old plugin");
    write(app / "settings.txt", "the user's");
    write(update / "ratcast.exe", "new");
    write(update / "qml" / "plugin.dll", "new plugin");
    write(update / "new.dll", "added");

    ASSERT_TRUE(replace_tree(update, app));
    EXPECT_EQ(read(app / "ratcast.exe"), "new");
    EXPECT_EQ(read(app / "qml" / "plugin.dll"), "new plugin");
    EXPECT_EQ(read(app / "new.dll"), "added");
    EXPECT_EQ(read(app / "settings.txt"), "the user's");
    EXPECT_EQ(read(aside(app / "ratcast.exe")), "old");

    remove_leftovers(app);
    EXPECT_FALSE(fs::exists(aside(app / "ratcast.exe")));
    EXPECT_FALSE(fs::exists(aside(app / "qml" / "plugin.dll")));
    EXPECT_TRUE(fs::exists(app / "ratcast.exe"));
}

TEST_F(InstallTest, AllIsPutBackWhenAFileCannotBeMoved) {
    const fs::path app = root_ / "app";
    const fs::path update = root_ / "update";
    write(app / "a.dll", "old a");
    write(app / "b.dll", "old b");
    write(app / "c.dll", "old c");
    write(update / "a.dll", "new a");
    write(update / "b.dll", "new b");
    write(update / "c.dll", "new c");
    write(update / "d.dll", "new d");

    // The third file in place of an old one is held by some other process.
    int moves = 0;
    const Rename rename = [&](const fs::path& from, const fs::path& to, std::error_code& error) {
        if (from.parent_path() == update && ++moves == 3) {
            error = std::make_error_code(std::errc::permission_denied);
            return;
        }
        fs::rename(from, to, error);
    };
    EXPECT_FALSE(replace_tree(update, app, rename));
    for (const char* name : {"a.dll", "b.dll", "c.dll"}) {
        EXPECT_EQ(read(app / name), std::string("old ") + name[0]) << name;
        EXPECT_FALSE(fs::exists(aside(app / name))) << name;
    }
    EXPECT_FALSE(fs::exists(app / "d.dll"));
}

TEST_F(InstallTest, AStagedUpdateIsAppliedOnce) {
    const fs::path app = root_ / "app";
    write(app / "ratcast.exe", "old");
    write(staging(app) / "files" / "ratcast-0.2.0-windows-x64" / "ratcast.exe", "new");

    EXPECT_FALSE(*apply_staged(app)) << "nothing staged yet";
    const std::optional<fs::path> found = find_root(staging(app) / "files", "ratcast.exe");
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, staging(app) / "files" / "ratcast-0.2.0-windows-x64");
    ASSERT_TRUE(stage(app, *found));

    const ratcast::Result<bool> applied = apply_staged(app);
    ASSERT_TRUE(applied);
    EXPECT_TRUE(*applied);
    EXPECT_EQ(read(app / "ratcast.exe"), "new");
    EXPECT_FALSE(fs::exists(staging(app)));
    EXPECT_FALSE(*apply_staged(app));
}

TEST_F(InstallTest, NothingFromOutsideItsStaging) {
    const fs::path app = root_ / "app";
    write(app / "ratcast.exe", "old");
    write(root_ / "elsewhere" / "ratcast.exe", "not an update");
    write(staging(app) / "ready", "../../elsewhere\n");

    EXPECT_FALSE(apply_staged(app));
    EXPECT_EQ(read(app / "ratcast.exe"), "old");
    EXPECT_FALSE(fs::exists(staging(app)));
}

TEST_F(InstallTest, TheArchivesFolderIsFound) {
    write(root_ / "flat" / "ratcast.exe", "");
    EXPECT_EQ(find_root(root_ / "flat", "ratcast.exe"), root_ / "flat");

    write(root_ / "nested" / "ratcast-0.2.0" / "ratcast.exe", "");
    EXPECT_EQ(find_root(root_ / "nested", "ratcast.exe"), root_ / "nested" / "ratcast-0.2.0");

    write(root_ / "none" / "readme.txt", "");
    EXPECT_FALSE(find_root(root_ / "none", "ratcast.exe"));

    write(root_ / "two" / "a" / "ratcast.exe", "");
    write(root_ / "two" / "b" / "ratcast.exe", "");
    EXPECT_FALSE(find_root(root_ / "two", "ratcast.exe")) << "which one?";

    // A macOS bundle: its program within it.
    const fs::path program = fs::path("Contents") / "MacOS" / "ratcast";
    write(root_ / "bundle" / "ratcast.app" / program, "");
    EXPECT_EQ(find_root(root_ / "bundle", program), root_ / "bundle" / "ratcast.app");
}

TEST_F(InstallTest, AFileIsReplacedAndMadeExecutable) {
    const fs::path appimage = root_ / "ratcast.AppImage";
    write(appimage, "old");
    const Target target{Target::Kind::File, appimage, appimage, "-linux-x86_64.AppImage"};
    const fs::path downloaded = download_path(target, "ratcast-0.2.0-linux-x86_64.AppImage");
    EXPECT_EQ(downloaded.parent_path(), root_);
    write(downloaded, "new");

    ASSERT_TRUE(replace_file(downloaded, appimage));
    EXPECT_EQ(read(appimage), "new");
    EXPECT_FALSE(fs::exists(downloaded));
#ifndef _WIN32
    EXPECT_NE(fs::status(appimage).permissions() & fs::perms::owner_exec, fs::perms::none);
#endif
}
