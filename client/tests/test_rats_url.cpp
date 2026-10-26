#include "ratcast/client/rats_url.h"

#include <gtest/gtest.h>

using namespace ratcast;

namespace {

TEST(RatsUrl, AnAddressToDial) {
    const std::optional<RatsUrl> v6 = parse_rats_url("rats://[::1]:7700/");
    ASSERT_TRUE(v6);
    EXPECT_EQ(std::get<librats::HostEndpoint>(*v6), (librats::HostEndpoint{"::1", 7700}));
    EXPECT_EQ(to_string(*v6), "rats://[::1]:7700");

    const std::optional<RatsUrl> named = parse_rats_url("rats://example.org:7700");
    ASSERT_TRUE(named);
    EXPECT_EQ(std::get<librats::HostEndpoint>(*named),
              (librats::HostEndpoint{"example.org", 7700}));
    EXPECT_EQ(to_string(*named), "rats://example.org:7700");
}

TEST(RatsUrl, APeerToLookFor) {
    const PeerId peer = PeerId::from_public_key(reinterpret_cast<const uint8_t*>("key"), 3);
    const std::optional<RatsUrl> found = parse_rats_url(to_string(RatsUrl{peer}));
    ASSERT_TRUE(found);
    EXPECT_EQ(std::get<PeerId>(*found), peer);
}

TEST(RatsUrl, RefusesTheRest) {
    for (const char* url : {"rats://", "rats://nonsense", "http://host:7700", "rats://::1:80",
                            "rats://host:0", "rats://host:70000", "rats://host", "host:7700"}) {
        EXPECT_FALSE(parse_rats_url(url)) << url;
    }
}

TEST(RatsUrl, ALinkNamesTheServersTheRoomIsMetAt) {
    using librats::HostEndpoint;
    const PeerId peer = PeerId::from_public_key(reinterpret_cast<const uint8_t*>("key"), 3);
    const RatsLink link{RatsUrl{peer},
                        {HostEndpoint{"degitx.com", 7700}, HostEndpoint{"2001:db8::1", 7701}}};
    const std::string text = to_string(link);
    EXPECT_EQ(text, "rats://" + peer.to_hex() + "?via=degitx.com:7700&via=[2001:db8::1]:7701");
    EXPECT_EQ(parse_rats_link(text), link);

    const std::optional<RatsLink> plain = parse_rats_link("rats://example.org:7700/");
    ASSERT_TRUE(plain);
    EXPECT_EQ(plain->url, RatsUrl{(HostEndpoint{"example.org", 7700})});
    EXPECT_TRUE(plain->via.empty());
    EXPECT_EQ(to_string(*plain), "rats://example.org:7700");

    // The port a server listens on unless told otherwise; another parameter,
    // one from a later version, passed over.
    const std::optional<RatsLink> short_via =
        parse_rats_link("rats://" + peer.to_hex() + "/?key=abc&via=degitx.com&");
    ASSERT_TRUE(short_via);
    EXPECT_EQ(short_via->via, (std::vector{HostEndpoint{"degitx.com", kServerPort}}));

    for (const std::string bad : {"rats://nonsense?via=degitx.com", "rats://host:7700?via=",
                                  "rats://host:7700?via=a/b", "?via=degitx.com"}) {
        EXPECT_FALSE(parse_rats_link(bad)) << bad;
    }
    EXPECT_FALSE(parse_rats_url(text)) << "a link's servers in a bare url";
}

TEST(RatsUrl, AServerAsAUserWritesOne) {
    using librats::HostEndpoint;
    EXPECT_EQ(parse_server("degitx.com"), (HostEndpoint{"degitx.com", kServerPort}));
    EXPECT_EQ(parse_server("degitx.com:9000"), (HostEndpoint{"degitx.com", 9000}));
    EXPECT_EQ(parse_server("rats://degitx.com/"), (HostEndpoint{"degitx.com", kServerPort}));
    EXPECT_EQ(parse_server("rats://203.0.113.5:7701"), (HostEndpoint{"203.0.113.5", 7701}));
    EXPECT_EQ(parse_server("[2001:db8::1]"), (HostEndpoint{"2001:db8::1", kServerPort}));
    EXPECT_EQ(parse_server("[2001:db8::1]:7701"), (HostEndpoint{"2001:db8::1", 7701}));
    for (const char* text : {"", "rats://", "2001:db8::1", "host:0", "host:port", "a/b", "[]"}) {
        EXPECT_FALSE(parse_server(text)) << text;
    }
}

}  // namespace
