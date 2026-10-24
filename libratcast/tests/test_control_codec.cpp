#include "wire/control_codec.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "wire/message.h"

using namespace ratcast;
using namespace ratcast::wire;
using namespace std::chrono_literals;

namespace {

StreamDescription screen() {
    StreamDescription d;
    d.id = 7;
    d.codec = Codec::AV1;
    d.format = VideoFormat{2560, 1440, 60, 1};
    d.lifetime = 150ms;
    d.label = "screen";
    const std::array<uint8_t, 4> av1c = {0x81, 0x08, 0x0C, 0x00};
    d.codec_config = Buffer::copy_of(av1c);
    return d;
}

StreamDescription microphone() {
    StreamDescription d;
    d.id = 70000;
    d.codec = Codec::Opus;
    d.format = AudioFormat{48000, 2};
    d.lifetime = 100ms;
    d.label = "mic";
    return d;
}

StreamDescription feed() {
    StreamDescription d;
    d.id = 2;
    d.codec = Codec::MpegTs;
    d.format = DataFormat{};
    d.lifetime = 1s;
    d.label = "contribution";
    return d;
}

std::vector<uint8_t> bytes_of(const Buffer& buffer) {
    return {buffer.data(), buffer.data() + buffer.size()};
}

Errc decode_error(const std::vector<uint8_t>& bytes) {
    const Result<ControlMessage> message = decode_control(bytes);
    EXPECT_FALSE(message);
    return message ? Errc{} : message.error();
}

template <typename Message>
Message round_trip(const Message& message) {
    const Buffer encoded = encode_control(message);
    Result<ControlMessage> decoded = decode_control(encoded.bytes());
    EXPECT_TRUE(decoded) << to_string(decoded.error());
    EXPECT_TRUE(std::holds_alternative<Message>(*decoded));
    return std::get<Message>(*decoded);
}

}  // namespace

TEST(ControlCodec, RoundTripsAnnouncements) {
    for (const StreamDescription& d : {screen(), microphone(), feed()}) {
        EXPECT_EQ(round_trip(Announce{d}).description, d);
    }
}

TEST(ControlCodec, RoundTripsStreamMessages) {
    EXPECT_EQ(round_trip(Withdraw{3}).stream, 3u);
    EXPECT_EQ(round_trip(Subscribe{0}).stream, 0u);
    EXPECT_EQ(round_trip(Unsubscribe{UINT32_MAX}).stream, UINT32_MAX);
    EXPECT_EQ(round_trip(KeyframeRequest{12345}).stream, 12345u);
}

TEST(ControlCodec, CarriesCodecsItDoesNotKnow) {
    StreamDescription d = screen();
    d.codec = static_cast<Codec>(200);
    EXPECT_EQ(round_trip(Announce{d}).description.codec, static_cast<Codec>(200));
}

TEST(ControlCodec, IgnoresFieldsAppendedByALaterVersion) {
    auto bytes = bytes_of(encode_control(Announce{screen()}));
    bytes.insert(bytes.end(), {0xDE, 0xAD});
    const Result<ControlMessage> message = decode_control(bytes);
    ASSERT_TRUE(message);
    EXPECT_EQ(std::get<Announce>(*message).description, screen());

    auto subscribe = bytes_of(encode_control(Subscribe{9}));
    subscribe.push_back(0x01);
    EXPECT_TRUE(decode_control(subscribe));
}

TEST(ControlCodec, EveryCutOfAnAnnouncementIsTruncated) {
    const auto bytes = bytes_of(encode_control(Announce{screen()}));
    for (size_t cut = 0; cut < bytes.size(); ++cut) {
        EXPECT_EQ(decode_error({bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(cut)}),
                  Errc::Truncated)
            << "cut at " << cut;
    }
}

TEST(ControlCodec, RefusesAnUnknownMediaKind) {
    auto bytes = bytes_of(encode_control(Announce{screen()}));
    bytes[3] = 9;  // prefix, stream, codec, then the media kind
    EXPECT_EQ(decode_error(bytes), Errc::Unsupported);
}

TEST(ControlCodec, RefusesAnInvalidDescription) {
    // A video codec announced as audio.
    auto bytes = bytes_of(encode_control(Announce{microphone()}));
    bytes[4] = static_cast<uint8_t>(Codec::H264);  // the stream id takes three bytes here
    EXPECT_EQ(decode_error(bytes), Errc::Malformed);
}

TEST(ControlCodec, RefusesOversizedFields) {
    std::vector<uint8_t> bytes = {message_prefix(MessageKind::Announce), 1,
                                  static_cast<uint8_t>(Codec::Opus),
                                  static_cast<uint8_t>(MediaKind::Audio), 100};
    bytes.push_back(StreamDescription::kMaxLabelSize + 1);  // label length
    bytes.resize(bytes.size() + StreamDescription::kMaxLabelSize + 1, 'x');
    EXPECT_EQ(decode_error(bytes), Errc::Malformed);
}

TEST(ControlCodec, RoundTripsPresence) {
    for (const Presence& p : {Presence{"Анна", false, false}, Presence{"", true, false},
                              Presence{"李明 🎮", false, true}, Presence{"bob", true, true}}) {
        EXPECT_EQ(round_trip(PresenceUpdate{p}).presence, p) << p.name;
    }
}

TEST(ControlCodec, EveryCutOfAPresenceIsTruncated) {
    const auto bytes = bytes_of(encode_control(PresenceUpdate{{"Анна", true, false}}));
    for (size_t cut = 0; cut < bytes.size(); ++cut) {
        EXPECT_EQ(decode_error({bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(cut)}),
                  Errc::Truncated)
            << "cut at " << cut;
    }
}

TEST(ControlCodec, RefusesANameThatIsNoName) {
    const auto with_name = [](std::vector<uint8_t> name) {
        std::vector<uint8_t> bytes = {message_prefix(MessageKind::Presence),
                                      static_cast<uint8_t>(name.size())};
        bytes.insert(bytes.end(), name.begin(), name.end());
        bytes.push_back(0);  // flags
        return bytes;
    };
    EXPECT_EQ(decode_error(with_name({'a', 0xFF, 'b'})), Errc::Malformed);  // not UTF-8
    EXPECT_EQ(decode_error(with_name({'a', '\n', 'b'})), Errc::Malformed);  // a control
    EXPECT_EQ(decode_error(with_name(std::vector<uint8_t>(Presence::kMaxNameSize + 1, 'x'))),
              Errc::Malformed);
}

TEST(ControlCodec, RoundTripsClocks) {
    const ClockProbe probe = round_trip(ClockProbe{Timestamp{1'234'567'890'123us}});
    EXPECT_EQ(probe.sent, Timestamp{1'234'567'890'123us});
    // A clock before its epoch: the bits come back as they went.
    const ClockEcho echo = round_trip(ClockEcho{Timestamp{-5s}, Timestamp{1'700'000'000s}});
    EXPECT_EQ(echo.sent, Timestamp{-5s});
    EXPECT_EQ(echo.at, Timestamp{1'700'000'000s});
}

TEST(ControlCodec, RoundTripsPlayout) {
    EXPECT_EQ(round_trip(PlayoutNeed{180ms}).latency, 180ms);
    EXPECT_EQ(round_trip(PlayoutNeed{}).latency, std::nullopt);
    EXPECT_EQ(round_trip(PlayoutDelay{kMaxPlayout}).delay, kMaxPlayout);
    EXPECT_EQ(round_trip(PlayoutDelay{0us}).delay, 0us);
    EXPECT_EQ(round_trip(PlayoutDelay{}).delay, std::nullopt);
}

TEST(ControlCodec, EveryCutOfAClockOrAPlayoutIsTruncated) {
    for (const ControlMessage& message :
         {ControlMessage{ClockEcho{Timestamp{100s}, Timestamp{200s}}},
          ControlMessage{PlayoutNeed{180ms}}, ControlMessage{PlayoutDelay{2s}}}) {
        const auto bytes = bytes_of(encode_control(message));
        for (size_t cut = 0; cut < bytes.size(); ++cut) {
            EXPECT_EQ(decode_error({bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(cut)}),
                      Errc::Truncated)
                << "kind " << int(bytes[0]) << ", cut at " << cut;
        }
    }
}

TEST(ControlCodec, RefusesAPlayoutPastItsMost) {
    auto bytes = bytes_of(encode_control(PlayoutDelay{kMaxPlayout}));
    ++bytes.back();  // the varint's highest seven bits, last
    EXPECT_EQ(decode_error(bytes), Errc::Malformed);
    EXPECT_EQ(decode_error({message_prefix(MessageKind::PlayoutNeed), 2, 0}), Errc::Malformed);
}

TEST(ControlCodec, AFrameIsNotAControlMessage) {
    EXPECT_EQ(decode_error({message_prefix(MessageKind::Frame), 0, 0, 0, 0}), Errc::UnknownKind);
    EXPECT_EQ(decode_error({message_prefix(MessageKind::RelayedFrame), 0}), Errc::UnknownKind);
}

PeerId peer(uint8_t seed) {
    std::array<uint8_t, 32> key{};
    key.fill(seed);
    return PeerId::from_public_key(key.data(), key.size());
}

TEST(ControlCodec, RoundTripsForward) {
    const Forward forward = round_trip(Forward{70000, 8'000'000, {peer(1), peer(2)}});
    EXPECT_EQ(forward.stream, 70000u);
    EXPECT_EQ(forward.most, 8'000'000u);
    EXPECT_EQ(forward.viewers, (std::vector{peer(1), peer(2)}));
    EXPECT_TRUE(round_trip(Forward{7, 0, {}}).viewers.empty()) << "to nobody";
}

TEST(ControlCodec, RoundTripsRelayedPath) {
    RelayedPath path{7, peer(3), {}, true};
    path.signal.expired = true;
    path.signal.queue_delay = 12ms;
    path.signal.rtt = 48ms;
    path.signal.queue_rise = 9ms;
    path.signal.capacity = 3'000'000;
    RelayedPath got = round_trip(path);
    EXPECT_EQ(got.stream, 7u);
    EXPECT_EQ(got.viewer, peer(3));
    EXPECT_TRUE(got.backpressured);
    EXPECT_FALSE(got.signal.starting);
    EXPECT_TRUE(got.signal.expired);
    EXPECT_EQ(got.signal.queue_delay, 12ms);
    EXPECT_EQ(got.signal.rtt, 48ms);
    EXPECT_EQ(got.signal.queue_rise, 9ms);
    EXPECT_EQ(got.signal.capacity, 3'000'000u);
    EXPECT_EQ(got.signal.delivered, std::nullopt);
    EXPECT_EQ(got.signal.carried, std::nullopt);

    path = RelayedPath{7, peer(3), {}, false};
    path.signal.starting = true;
    path.signal.delivered = 900'000;
    path.signal.carried = 1'100'000;
    got = round_trip(path);
    EXPECT_TRUE(got.signal.starting);
    EXPECT_FALSE(got.backpressured);
    EXPECT_EQ(got.signal.capacity, std::nullopt);
    EXPECT_EQ(got.signal.delivered, 900'000u);
    EXPECT_EQ(got.signal.carried, 1'100'000u);
}

TEST(ControlCodec, RoundTripsCannotReach) {
    const CannotReach cannot = round_trip(CannotReach{7, peer(4)});
    EXPECT_EQ(cannot.stream, 7u);
    EXPECT_EQ(cannot.viewer, peer(4));
}

TEST(ControlCodec, RoundTripsUplink) {
    EXPECT_EQ(round_trip(Uplink{6'000'000}).capacity, 6'000'000u);
    EXPECT_EQ(round_trip(Uplink{}).capacity, std::nullopt);
    EXPECT_EQ(decode_error({message_prefix(MessageKind::Uplink), 2}), Errc::Malformed);
}

TEST(ControlCodec, RoundTripsForwarder) {
    const auto bytes = bytes_of(encode_control(Forwarder{}));
    EXPECT_EQ(bytes, std::vector<uint8_t>{message_prefix(MessageKind::Forwarder)});
    const Result<ControlMessage> decoded = decode_control(bytes);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(std::holds_alternative<Forwarder>(*decoded));
}

TEST(ControlCodec, RoundTripsFrameKey) {
    FrameKey key{300, {}};
    for (size_t i = 0; i < key.key.size(); ++i) key.key[i] = static_cast<uint8_t>(i * 7);
    const FrameKey got = round_trip(key);
    EXPECT_EQ(got.kid, 300u);
    EXPECT_EQ(got.key, key.key);
    const auto bytes = bytes_of(encode_control(key));
    for (size_t cut = 0; cut < bytes.size(); ++cut) {
        EXPECT_EQ(decode_error({bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(cut)}),
                  Errc::Truncated);
    }
}

TEST(ControlCodec, EveryCutOfARelayMessageIsTruncated) {
    RelayedPath path{7, peer(3), {}, false};
    path.signal.capacity = 3'000'000;
    path.signal.delivered = 2'000'000;
    path.signal.carried = 2'500'000;
    for (const ControlMessage& message :
         {ControlMessage{Forward{7, 8'000'000, {peer(1), peer(2)}}}, ControlMessage{path},
          ControlMessage{Uplink{6'000'000}}, ControlMessage{CannotReach{7, peer(4)}}}) {
        const auto bytes = bytes_of(encode_control(message));
        for (size_t cut = 0; cut < bytes.size(); ++cut) {
            EXPECT_EQ(decode_error({bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(cut)}),
                      Errc::Truncated)
                << "kind " << int(bytes[0]) << ", cut at " << cut;
        }
    }
}

TEST(ControlCodec, RefusesAForwardToTooManyOrAPathWithFlagsItDoesNotKnow) {
    EXPECT_EQ(decode_error({message_prefix(MessageKind::Forward), 7, 0, kMaxForwarded + 1}),
              Errc::Malformed);
    auto bytes = bytes_of(encode_control(RelayedPath{7, peer(3), {}, false}));
    bytes[2 + PeerId::kSize] = 64;  // the flags, past the last known
    EXPECT_EQ(decode_error(bytes), Errc::Malformed);
}
