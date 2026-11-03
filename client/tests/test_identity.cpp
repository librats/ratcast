#include "ratcast/client/identity.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

using namespace ratcast;

namespace {

PeerId peer_of(const char* name) {
    return PeerId::from_public_key(reinterpret_cast<const uint8_t*>(name), std::strlen(name));
}

std::filesystem::path temp_file(const char* name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("ratcast-identity-" + to_hex(random_bytes<8>())) / name;
    return path;
}

TEST(Identity, BytesWrittenAndReadBack) {
    const std::vector<uint8_t> bytes = {0, 1, 0x7f, 0x80, 0xfe, 0xff, 42};
    EXPECT_EQ(to_hex(bytes), "00017f80feff2a");
    EXPECT_EQ(from_hex("00017F80FEFF2a", bytes.size()), bytes);
    EXPECT_FALSE(from_hex("00017f80feff2", bytes.size()));
    EXPECT_FALSE(from_hex("00017f80feff2g", bytes.size()));
    for (size_t size = 0; size <= bytes.size(); ++size) {
        const std::vector<uint8_t> some(bytes.begin(),
                                        bytes.begin() + static_cast<ptrdiff_t>(size));
        const std::string text = to_base64url(some);
        EXPECT_EQ(from_base64url(text, size), some) << text;
        EXPECT_FALSE(from_base64url(text + "A", size)) << text;
    }
    EXPECT_EQ(to_base64url(std::vector<uint8_t>{0xfb, 0xff}), "-_8");
    EXPECT_FALSE(from_base64url("-_9", 2)) << "bits past the last byte";
    EXPECT_FALSE(from_base64url("-_+", 2));
}

TEST(Identity, AKeySignsAndOthersCheck) {
    const PersonKey ann = PersonKey::generate();
    const PersonKey bob = PersonKey::generate();
    EXPECT_NE(ann.id(), bob.id());
    const std::vector<uint8_t> message = {1, 2, 3};
    const std::array<uint8_t, 64> signature = ann.sign(message);
    EXPECT_TRUE(verify(ann.id(), message, signature));
    EXPECT_FALSE(verify(bob.id(), message, signature));
    EXPECT_FALSE(verify(ann.id(), std::vector<uint8_t>{1, 2, 4}, signature));

    // The same key from its seed, from its text.
    EXPECT_EQ(PersonKey::from_seed(ann.seed()).id(), ann.id());
    const std::optional<PersonKey> read = PersonKey::from_text(ann.to_text());
    ASSERT_TRUE(read);
    EXPECT_EQ(read->id(), ann.id());
    EXPECT_FALSE(PersonKey::from_text("ratcast person key\nnot hex\n"));
    EXPECT_FALSE(PersonKey::from_text("something else\n" + to_hex(ann.seed()) + "\n"));
}

TEST(Identity, AKeyIsKeptInAFile) {
    const std::filesystem::path path = temp_file("person.key");
    EXPECT_EQ(PersonKey::load(path).error(), Errc::Unavailable);
    const Result<PersonKey> made = PersonKey::load_or_create(path);
    ASSERT_TRUE(made);
    const Result<PersonKey> kept = PersonKey::load_or_create(path);
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->id(), made->id()) << "the same one, the next run";
    {
        std::ofstream broken(path, std::ios::trunc);
        broken << "nonsense";
    }
    EXPECT_EQ(PersonKey::load(path).error(), Errc::Malformed);
    EXPECT_FALSE(PersonKey::load_or_create(path)) << "a broken key is never replaced";
    std::filesystem::remove_all(path.parent_path());
}

TEST(Identity, ATagTellsTwoApart) {
    std::set<std::string> tags;
    for (int i = 0; i < 64; ++i) {
        const std::string tag = PersonKey::generate().id().tag();
        ASSERT_EQ(tag.size(), 4u);
        for (const char c : tag) EXPECT_EQ(std::string("ILOU").find(c), std::string::npos) << tag;
        tags.insert(tag);
    }
    EXPECT_GT(tags.size(), 60u) << "a million tags: few alike of 64";
    const PersonKey ann = PersonKey::generate();
    EXPECT_EQ(ann.id().tag(), PersonKey::from_seed(ann.seed()).id().tag());
}

TEST(Identity, ADeviceIsCertified) {
    const PersonKey ann = PersonKey::generate();
    const PeerId laptop = peer_of("laptop");
    const Certificate certificate = certify(ann, laptop);
    EXPECT_TRUE(is_valid(certificate));
    EXPECT_EQ(certificate.person, ann.id());
    EXPECT_EQ(certificate.device, laptop);

    Certificate forged = certificate;
    forged.device = peer_of("phone");
    EXPECT_FALSE(is_valid(forged)) << "another device";
    forged = certificate;
    forged.person = PersonKey::generate().id();
    EXPECT_FALSE(is_valid(forged)) << "another person";
    forged = certificate;
    forged.issued += 1;
    EXPECT_FALSE(is_valid(forged)) << "another time";
}

TEST(Identity, ACardSaysWhoAPeerIs) {
    const PersonKey ann = PersonKey::generate();
    const PeerId laptop = peer_of("laptop");
    IdentityCard card{.certificate = certify(ann, laptop), .request_secret = random_bytes<16>()};
    const std::vector<uint8_t> bytes = encode(card);
    EXPECT_EQ(bytes.size(), IdentityCard::kSize);
    EXPECT_EQ(decode_identity(bytes), card);
    EXPECT_EQ(identity_of(bytes, laptop), card);
    EXPECT_FALSE(identity_of(bytes, peer_of("phone"))) << "told by another peer";

    std::vector<uint8_t> longer = bytes;
    longer.push_back(9);
    EXPECT_EQ(decode_identity(longer), card) << "a later version's, longer";
    for (size_t cut = 0; cut < bytes.size(); ++cut)
        EXPECT_FALSE(decode_identity({bytes.data(), cut})) << cut;
    std::vector<uint8_t> tampered = bytes;
    tampered[40] ^= 1;
    EXPECT_FALSE(decode_identity(tampered));
    tampered = bytes;
    tampered[0] = 2;
    EXPECT_FALSE(decode_identity(tampered)) << "another version";
}

TEST(Identity, AMacIsTheKeys) {
    const std::array<uint8_t, 16> key = random_bytes<16>();
    const std::vector<uint8_t> message = {1, 2, 3};
    const auto a = mac(key, message);
    EXPECT_EQ(mac(key, message), a);
    EXPECT_NE(mac(random_bytes<16>(), message), a);
    EXPECT_NE(mac(key, std::vector<uint8_t>{1, 2}), a);
    EXPECT_TRUE(same_secret(a, mac(key, message)));
    EXPECT_FALSE(same_secret(a, mac(key, std::vector<uint8_t>{1})));
    EXPECT_FALSE(same_secret(a, std::span(a).first(16)));
}

}  // namespace
