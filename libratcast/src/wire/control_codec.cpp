#include "wire/control_codec.h"

#include <cassert>
#include <limits>
#include <string>
#include <type_traits>

#include "wire/bytes.h"
#include "wire/message.h"

namespace ratcast::wire {
namespace {

/// Stands in for a Writer to measure a message before it is written, so both
/// passes run the same code and cannot disagree.
class Sizer {
public:
    void u8(uint8_t) noexcept { size_ += 1; }
    void varint(uint64_t value) noexcept { size_ += varint_size(value); }
    void bytes(std::span<const uint8_t> bytes) noexcept { size_ += bytes.size(); }
    size_t size() const noexcept { return size_; }

private:
    size_t size_ = 0;
};

std::span<const uint8_t> as_bytes(const std::string& text) noexcept {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

template <typename Out>
void write(Out& out, const Announce& announce) {
    const StreamDescription& d = announce.description;
    out.u8(message_prefix(MessageKind::Announce));
    out.varint(d.id);
    out.u8(static_cast<uint8_t>(d.codec));
    out.u8(static_cast<uint8_t>(d.kind()));
    out.varint(static_cast<uint64_t>(d.lifetime.count()));
    out.varint(d.label.size());
    out.bytes(as_bytes(d.label));
    if (const VideoFormat* video = d.video()) {
        out.varint(video->width);
        out.varint(video->height);
        out.varint(video->frame_rate_num);
        out.varint(video->frame_rate_den);
    } else if (const AudioFormat* audio = d.audio()) {
        out.varint(audio->sample_rate);
        out.u8(audio->channels);
    }  // a data stream's format has no fields
    out.varint(d.codec_config.size());
    out.bytes(d.codec_config.bytes());
}

enum PresenceFlags : uint8_t { kMuted = 1, kDeafened = 2 };

template <typename Out>
void write(Out& out, const PresenceUpdate& update) {
    const Presence& p = update.presence;
    out.u8(message_prefix(MessageKind::Presence));
    out.varint(p.name.size());
    out.bytes(as_bytes(p.name));
    out.u8(static_cast<uint8_t>((p.muted ? kMuted : 0) | (p.deafened ? kDeafened : 0)));
}

template <typename Out, typename Message>
void write(Out& out, const Message& message) {
    constexpr MessageKind kind = [] {
        if constexpr (std::is_same_v<Message, Withdraw>) return MessageKind::Withdraw;
        if constexpr (std::is_same_v<Message, Subscribe>) return MessageKind::Subscribe;
        if constexpr (std::is_same_v<Message, Unsubscribe>) return MessageKind::Unsubscribe;
        if constexpr (std::is_same_v<Message, KeyframeRequest>) return MessageKind::KeyframeRequest;
    }();
    out.u8(message_prefix(kind));
    out.varint(message.stream);
}

uint32_t read_u32(Reader& in) noexcept {
    const uint64_t value = in.varint();
    if (value > std::numeric_limits<uint32_t>::max()) in.fail(Errc::Malformed);
    return static_cast<uint32_t>(value);
}

Result<ControlMessage> read_announce(Reader& in) {
    StreamDescription d;
    d.id = read_u32(in);
    d.codec = static_cast<Codec>(in.u8());
    const auto kind = static_cast<MediaKind>(in.u8());

    const uint64_t lifetime = in.varint();
    if (lifetime > static_cast<uint64_t>(StreamDescription::kMaxLifetime.count())) {
        in.fail(Errc::Malformed);
    }
    d.lifetime = Duration{static_cast<Duration::rep>(lifetime)};

    const uint64_t label_size = in.varint();
    if (label_size > StreamDescription::kMaxLabelSize) in.fail(Errc::Malformed);
    const std::span<const uint8_t> label = in.bytes(static_cast<size_t>(label_size));
    d.label.assign(label.begin(), label.end());

    switch (kind) {
        case MediaKind::Video: {
            VideoFormat video;
            video.width = read_u32(in);
            video.height = read_u32(in);
            video.frame_rate_num = read_u32(in);
            video.frame_rate_den = read_u32(in);
            d.format = video;
            break;
        }
        case MediaKind::Audio: {
            AudioFormat audio;
            audio.sample_rate = read_u32(in);
            audio.channels = in.u8();
            d.format = audio;
            break;
        }
        case MediaKind::Data: d.format = DataFormat{}; break;
        default:
            if (!in.ok()) return fail(in.error());
            return fail(Errc::Unsupported);
    }

    const uint64_t config_size = in.varint();
    if (config_size > StreamDescription::kMaxCodecConfigSize) in.fail(Errc::Malformed);
    const std::span<const uint8_t> config = in.bytes(static_cast<size_t>(config_size));

    if (!in.ok()) return fail(in.error());
    d.codec_config = Buffer::copy_of(config);
    if (!is_valid(d)) return fail(Errc::Malformed);
    return ControlMessage{Announce{std::move(d)}};
}

Result<ControlMessage> read_presence(Reader& in) {
    Presence p;
    const uint64_t name_size = in.varint();
    if (name_size > Presence::kMaxNameSize) in.fail(Errc::Malformed);
    const std::span<const uint8_t> name = in.bytes(static_cast<size_t>(name_size));
    const uint8_t flags = in.u8();
    if (!in.ok()) return fail(in.error());
    p.name.assign(name.begin(), name.end());
    p.muted = flags & kMuted;
    p.deafened = flags & kDeafened;
    if (!is_valid(p)) return fail(Errc::Malformed);
    return ControlMessage{PresenceUpdate{std::move(p)}};
}

template <typename Message>
Result<ControlMessage> read_stream_message(Reader& in) {
    Message message;
    message.stream = read_u32(in);
    if (!in.ok()) return fail(in.error());
    return ControlMessage{message};
}

}  // namespace

Buffer encode_control(const ControlMessage& message) {
    return std::visit(
        [](const auto& m) {
            if constexpr (std::is_same_v<std::decay_t<decltype(m)>, Announce>) {
                assert(is_valid(m.description));
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, PresenceUpdate>) {
                assert(is_valid(m.presence));
            }
            Sizer sizer;
            write(sizer, m);
            MutableBuffer buffer = MutableBuffer::allocate(sizer.size());
            Writer out(std::span(buffer.append(sizer.size()), sizer.size()));
            write(out, m);
            assert(out.position() == sizer.size());
            return std::move(buffer).freeze();
        },
        message);
}

Result<ControlMessage> decode_control(std::span<const uint8_t> message) {
    Reader in(message);
    const Result<MessageKind> kind = read_prefix(in);
    if (!kind) return fail(kind.error());

    switch (*kind) {
        case MessageKind::Announce: return read_announce(in);
        case MessageKind::Withdraw: return read_stream_message<Withdraw>(in);
        case MessageKind::Subscribe: return read_stream_message<Subscribe>(in);
        case MessageKind::Unsubscribe: return read_stream_message<Unsubscribe>(in);
        case MessageKind::KeyframeRequest: return read_stream_message<KeyframeRequest>(in);
        case MessageKind::Presence: return read_presence(in);
        case MessageKind::Frame: break;
    }
    return fail(Errc::UnknownKind);  // not a control message
}

}  // namespace ratcast::wire
