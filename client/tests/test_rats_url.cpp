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

}  // namespace
