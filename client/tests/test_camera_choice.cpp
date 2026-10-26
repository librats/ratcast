// Whose cameras a side shows when it shows only so many: the last to speak,
// those shown keeping their places until someone speaks after them.

#include "ratcast/client/camera_choice.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>

using namespace ratcast;
using namespace std::chrono_literals;

namespace {

PeerId peer(uint8_t seed) {
    std::array<uint8_t, 32> key{};
    key.fill(seed);
    return PeerId::from_public_key(key.data(), key.size());
}

Timestamp at(int seconds) { return Timestamp{std::chrono::seconds(seconds)}; }

CameraOffer offer(uint8_t seed, std::optional<Timestamp> spoke = std::nullopt, bool shown = false) {
    return {peer(seed), spoke, shown};
}

/// In one order, to compare as sets.
std::vector<PeerId> sorted(std::vector<PeerId> peers) {
    std::sort(peers.begin(), peers.end());
    return peers;
}

std::vector<PeerId> peers(std::initializer_list<uint8_t> seeds) {
    std::vector<PeerId> out;
    for (const uint8_t seed : seeds) out.push_back(peer(seed));
    return sorted(out);
}

}  // namespace

TEST(CameraChoice, ShowsEveryCameraWhenThereAreNoMoreThanItShows) {
    const std::array offers{offer(1), offer(2, at(5)), offer(3)};
    EXPECT_EQ(sorted(choose_cameras(offers, 3)), peers({1, 2, 3}));
    EXPECT_EQ(sorted(choose_cameras(offers, 0)), peers({1, 2, 3})) << "no limit";
    EXPECT_TRUE(choose_cameras({}, 2).empty());
}

TEST(CameraChoice, ShowsTheLastToSpeak) {
    const std::array offers{offer(1, at(1)), offer(2, at(9)), offer(3), offer(4, at(5))};
    EXPECT_EQ(sorted(choose_cameras(offers, 2)), peers({2, 4}));
}

TEST(CameraChoice, WhileNobodySpeaksTheFirstOfferedAreShown) {
    const std::array offers{offer(3), offer(1), offer(2)};
    EXPECT_EQ(sorted(choose_cameras(offers, 2)), peers({3, 1}));
}

TEST(CameraChoice, ACameraShownStaysAgainstOneThatNeverSpokeOrSpokeNoLater) {
    // 3, shown, never spoke: 1, offered first, never did either.
    const std::array silent{offer(1), offer(3, std::nullopt, true)};
    EXPECT_EQ(choose_cameras(silent, 1), peers({3}));
    // 4 was heard when 2, shown, was.
    const std::array together{offer(4, at(5)), offer(2, at(5), true)};
    EXPECT_EQ(choose_cameras(together, 1), peers({2}));
}

TEST(CameraChoice, OneThatSpeaksTakesThePlaceOfTheLongestSilent) {
    const std::array offers{offer(1, at(2), true), offer(2, at(7), true), offer(3, at(8))};
    EXPECT_EQ(sorted(choose_cameras(offers, 2)), peers({2, 3}));
}

TEST(CameraChoice, ACameraShownKeepsItsPlaceForAMomentAfterItsMemberStops) {
    static_assert(kKeepCamera == std::chrono::seconds(2));
    // Two talking at once: the one shown stays.
    EXPECT_EQ(choose_cameras(std::array{offer(1, at(5), true), offer(2, at(6))}, 1), peers({1}));
    EXPECT_EQ(choose_cameras(std::array{offer(1, at(5), true), offer(2, at(7))}, 1), peers({1}));
    // Heard past the moment after: in its place.
    EXPECT_EQ(choose_cameras(std::array{offer(1, at(5), true), offer(2, at(8))}, 1), peers({2}));
    EXPECT_EQ(choose_cameras(std::array{offer(1, at(5), true), offer(2, at(6))}, 1, 0s),
              peers({2}));
}
