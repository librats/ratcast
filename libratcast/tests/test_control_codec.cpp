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
    for (const StreamDescription& d : {screen(), microphone()}) {
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

TEST(ControlCodec, AFrameIsNotAControlMessage) {
    EXPECT_EQ(decode_error({message_prefix(MessageKind::Frame), 0, 0, 0, 0}), Errc::UnknownKind);
}
