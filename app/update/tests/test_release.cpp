// A GitHub release as the updater reads it: the version from its tag, the one
// package for this system (never the symbols beside it), its SHA-256; nothing
// from a prerelease, a draft, or a tag that is no version.

#include "release.h"

#include <gtest/gtest.h>

using namespace ratcast::app::update;

namespace {

constexpr char16_t kSuffix[] = u"-windows-x64.zip";

QByteArray release_json(const char* tag, bool prerelease = false, bool digest = true) {
    QByteArray json = R"({
        "tag_name": ")";
    json += tag;
    json += R"(",
        "html_url": "https://github.com/librats/ratcast/releases/tag/v0.2.0",
        "draft": false,
        "prerelease": )";
    json += prerelease ? "true" : "false";
    json += R"(,
        "assets": [
            {"name": "ratcast-0.2.0-windows-x64-symbols.zip", "size": 10,
             "browser_download_url": "https://github.com/x/symbols.zip"},
            {"name": "ratcast-0.2.0-linux-x86_64.AppImage", "size": 20,
             "browser_download_url": "https://github.com/x/ratcast.AppImage"},
            {"name": "ratcast-0.2.0-windows-x64.zip", "size": 30,
             "browser_download_url": "https://github.com/x/ratcast.zip")";
    if (digest) json += R"(, "digest": "sha256:00ff10")";
    json += "}]}";
    return json;
}

}  // namespace

TEST(ReleaseTest, ThePackageForThisSystemAndItsVersion) {
    const std::optional<Release> release =
        parse_release(release_json("v0.2.0"), u"-windows-x64.zip");
    ASSERT_TRUE(release);
    EXPECT_EQ(release->version, QVersionNumber(0, 2, 0));
    EXPECT_EQ(release->asset, "ratcast-0.2.0-windows-x64.zip");
    EXPECT_EQ(release->url, QUrl("https://github.com/x/ratcast.zip"));
    EXPECT_EQ(release->size, 30);
    EXPECT_EQ(release->sha256, QByteArray::fromHex("00ff10"));
    EXPECT_EQ(release->page, QUrl("https://github.com/librats/ratcast/releases/tag/v0.2.0"));

    const std::optional<Release> appimage =
        parse_release(release_json("v0.2.0"), u"-linux-x86_64.AppImage");
    ASSERT_TRUE(appimage);
    EXPECT_EQ(appimage->size, 20);
}

TEST(ReleaseTest, NoDigestIsNoHash) {
    const std::optional<Release> release =
        parse_release(release_json("v0.2.0", false, false), u"-windows-x64.zip");
    ASSERT_TRUE(release);
    EXPECT_TRUE(release->sha256.isEmpty());
}

TEST(ReleaseTest, NothingFromAPrereleaseOrATagThatIsNoVersion) {
    EXPECT_FALSE(parse_release(release_json("v0.2.0-rc1", true), kSuffix));
    EXPECT_FALSE(parse_release(release_json("v0.2.0-rc1"), kSuffix));
    EXPECT_FALSE(parse_release(release_json("nightly"), kSuffix));
    EXPECT_FALSE(parse_release("not json", kSuffix));
    EXPECT_FALSE(parse_release(R"({"tag_name": "v0.2.0", "draft": true, "assets": []})", kSuffix));
}

TEST(ReleaseTest, NothingWithoutThePackage) {
    EXPECT_FALSE(parse_release(release_json("v0.2.0"), u"-macos-arm64.zip"));
}
