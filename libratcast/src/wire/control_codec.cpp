#include "wire/control_codec.h"

#include <algorithm>
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
void write_text(Out& out, const std::string& text) {
    out.varint(text.size());
    out.bytes(as_bytes(text));
}

template <typename Out>
void write(Out& out, const Announce& announce) {
    const StreamDescription& d = announce.description;
    out.u8(message_prefix(MessageKind::Announce));
    out.varint(d.id);
    out.u8(static_cast<uint8_t>(d.codec));
    out.u8(static_cast<uint8_t>(d.kind()));
    out.varint(static_cast<uint64_t>(d.lifetime.count()));
    write_text(out, d.label);
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
    write_text(out, p.name);
    out.u8(static_cast<uint8_t>((p.muted ? kMuted : 0) | (p.deafened ? kDeafened : 0)));
}

// Times go as the bits of their microseconds: a probe's comes back as it went.
uint64_t bits_of(Timestamp time) noexcept {
    return static_cast<uint64_t>(time.time_since_epoch().count());
}

Timestamp time_of(uint64_t bits) noexcept {
    return Timestamp{Duration{static_cast<Duration::rep>(bits)}};
}

template <typename Out>
void write(Out& out, const ClockProbe& probe) {
    out.u8(message_prefix(MessageKind::ClockProbe));
    out.varint(bits_of(probe.sent));
}

template <typename Out>
void write(Out& out, const ClockEcho& echo) {
    out.u8(message_prefix(MessageKind::ClockEcho));
    out.varint(bits_of(echo.sent));
    out.varint(bits_of(echo.at));
}

template <typename Out>
void write_playout(Out& out, MessageKind kind, std::optional<Duration> value) {
    out.u8(message_prefix(kind));
    out.u8(value ? 1 : 0);
    if (value) out.varint(static_cast<uint64_t>(value->count()));
}

template <typename Out>
void write(Out& out, const PlayoutNeed& need) {
    write_playout(out, MessageKind::PlayoutNeed, need.latency);
}

template <typename Out>
void write(Out& out, const PlayoutDelay& delay) {
    write_playout(out, MessageKind::PlayoutDelay, delay.delay);
}

PeerId read_peer(Reader& in) {
    const std::span<const uint8_t> bytes = in.bytes(PeerId::kSize);
    if (!in.ok()) return PeerId{};
    return *PeerId::from_bytes(librats::ByteView(bytes.data(), bytes.size()));
}

template <typename Out>
void write(Out& out, const Forward& forward) {
    out.u8(message_prefix(MessageKind::Forward));
    out.varint(forward.stream);
    out.varint(forward.most);
    out.varint(forward.viewers.size());
    for (const PeerId& viewer : forward.viewers) out.bytes(viewer.bytes());
}

enum RelayedPathFlags : uint8_t {
    kStarting = 1,
    kExpired = 2,
    kBackpressured = 4,
    kCapacity = 8,
    kDelivered = 16,
    kCarried = 32,
};

template <typename Out>
void write(Out& out, const RelayedPath& path) {
    const PathSignal& s = path.signal;
    out.u8(message_prefix(MessageKind::RelayedPath));
    out.varint(path.stream);
    out.bytes(path.viewer.bytes());
    out.u8(static_cast<uint8_t>((s.starting ? kStarting : 0) | (s.expired ? kExpired : 0) |
                                (path.backpressured ? kBackpressured : 0) |
                                (s.capacity ? kCapacity : 0) | (s.delivered ? kDelivered : 0) |
                                (s.carried ? kCarried : 0)));
    out.varint(static_cast<uint64_t>(s.queue_delay.count()));
    out.varint(static_cast<uint64_t>(s.rtt.count()));
    out.varint(static_cast<uint64_t>(s.queue_rise.count()));
    if (s.capacity) out.varint(*s.capacity);
    if (s.delivered) out.varint(*s.delivered);
    if (s.carried) out.varint(*s.carried);
}

template <typename Out>
void write(Out& out, const Uplink& uplink) {
    out.u8(message_prefix(MessageKind::Uplink));
    out.u8(uplink.capacity ? 1 : 0);
    if (uplink.capacity) out.varint(*uplink.capacity);
}

template <typename Out>
void write(Out& out, const CannotReach& cannot) {
    out.u8(message_prefix(MessageKind::CannotReach));
    out.varint(cannot.stream);
    out.bytes(cannot.viewer.bytes());
}

template <typename Out>
void write(Out& out, const Forwarder&) {
    out.u8(message_prefix(MessageKind::Forwarder));
}

template <typename Out>
void write(Out& out, const FrameKey& key) {
    out.u8(message_prefix(MessageKind::FrameKey));
    out.varint(key.kid);
    out.bytes(key.key);
}

uint64_t bits_of(WallTime time) noexcept {
    return static_cast<uint64_t>(time.time_since_epoch().count());
}

template <typename Out>
void write(Out& out, const Chat& chat) {
    out.u8(message_prefix(MessageKind::Chat));
    out.varint(bits_of(chat.sent));
    write_text(out, chat.text);
}

template <typename Out>
void write(Out& out, const ChatAsk&) {
    out.u8(message_prefix(MessageKind::ChatAsk));
}

template <typename Out>
void write(Out& out, const ChatTold& told) {
    const ChatMessage& m = told.message;
    out.u8(message_prefix(MessageKind::ChatTold));
    out.bytes(m.author.bytes());
    out.varint(bits_of(m.sent));
    write_text(out, m.name);
    write_text(out, m.text);
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

std::string read_text(Reader& in, size_t most) {
    const uint64_t size = in.varint();
    if (size > most) in.fail(Errc::Malformed);
    const std::span<const uint8_t> text = in.bytes(static_cast<size_t>(size));
    if (!in.ok()) return {};
    return {text.begin(), text.end()};
}

WallTime read_wall_time(Reader& in) {
    const uint64_t us = in.varint();
    if (us > static_cast<uint64_t>(std::numeric_limits<WallTime::rep>::max())) {
        in.fail(Errc::Malformed);
    }
    return WallTime{WallTime::duration{static_cast<WallTime::rep>(us)}};
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

    d.label = read_text(in, StreamDescription::kMaxLabelSize);

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

Result<ControlMessage> read_forward(Reader& in) {
    Forward forward;
    forward.stream = read_u32(in);
    forward.most = read_u32(in);
    const uint64_t count = in.varint();
    if (count > kMaxForwarded) in.fail(Errc::Malformed);
    for (uint64_t i = 0; i < count && in.ok(); ++i) forward.viewers.push_back(read_peer(in));
    if (!in.ok()) return fail(in.error());
    return ControlMessage{std::move(forward)};
}

Duration read_duration(Reader& in) {
    const uint64_t us = in.varint();
    if (us > static_cast<uint64_t>(std::numeric_limits<Duration::rep>::max())) {
        in.fail(Errc::Malformed);
    }
    return Duration{static_cast<Duration::rep>(us)};
}

Result<ControlMessage> read_relayed_path(Reader& in) {
    RelayedPath path;
    path.stream = read_u32(in);
    path.viewer = read_peer(in);
    const uint8_t flags = in.u8();
    if (flags >= kCarried * 2) in.fail(Errc::Malformed);
    PathSignal& s = path.signal;
    s.starting = flags & kStarting;
    s.expired = flags & kExpired;
    path.backpressured = flags & kBackpressured;
    s.queue_delay = read_duration(in);
    s.rtt = read_duration(in);
    s.queue_rise = read_duration(in);
    if (flags & kCapacity) s.capacity = in.varint();
    if (flags & kDelivered) s.delivered = in.varint();
    if (flags & kCarried) s.carried = in.varint();
    if (!in.ok()) return fail(in.error());
    return ControlMessage{path};
}

Result<ControlMessage> read_uplink(Reader& in) {
    Uplink uplink;
    const uint8_t given = in.u8();
    if (given > 1) in.fail(Errc::Malformed);
    if (given == 1) uplink.capacity = in.varint();
    if (!in.ok()) return fail(in.error());
    return ControlMessage{uplink};
}

Result<ControlMessage> read_cannot_reach(Reader& in) {
    CannotReach cannot;
    cannot.stream = read_u32(in);
    cannot.viewer = read_peer(in);
    if (!in.ok()) return fail(in.error());
    return ControlMessage{cannot};
}

Result<ControlMessage> read_frame_key(Reader& in) {
    FrameKey key;
    key.kid = in.varint();
    const std::span<const uint8_t> bytes = in.bytes(key.key.size());
    if (!in.ok()) return fail(in.error());
    std::ranges::copy(bytes, key.key.begin());
    return ControlMessage{key};
}

Result<ControlMessage> read_presence(Reader& in) {
    Presence p;
    p.name = read_text(in, Presence::kMaxNameSize);
    const uint8_t flags = in.u8();
    if (!in.ok()) return fail(in.error());
    p.muted = flags & kMuted;
    p.deafened = flags & kDeafened;
    if (!is_valid(p)) return fail(Errc::Malformed);
    return ControlMessage{PresenceUpdate{std::move(p)}};
}

Result<ControlMessage> read_chat(Reader& in) {
    ChatMessage m;
    m.sent = read_wall_time(in);
    m.text = read_text(in, ChatMessage::kMaxTextSize);
    if (!in.ok()) return fail(in.error());
    if (!is_valid(m)) return fail(Errc::Malformed);
    return ControlMessage{Chat{m.sent, std::move(m.text)}};
}

Result<ControlMessage> read_chat_told(Reader& in) {
    ChatMessage m;
    m.author = read_peer(in);
    m.sent = read_wall_time(in);
    m.name = read_text(in, Presence::kMaxNameSize);
    m.text = read_text(in, ChatMessage::kMaxTextSize);
    if (!in.ok()) return fail(in.error());
    if (!is_valid(m)) return fail(Errc::Malformed);
    return ControlMessage{ChatTold{std::move(m)}};
}

Result<ControlMessage> read_clock_probe(Reader& in) {
    const ClockProbe probe{time_of(in.varint())};
    if (!in.ok()) return fail(in.error());
    return ControlMessage{probe};
}

Result<ControlMessage> read_clock_echo(Reader& in) {
    ClockEcho echo;
    echo.sent = time_of(in.varint());
    echo.at = time_of(in.varint());
    if (!in.ok()) return fail(in.error());
    return ControlMessage{echo};
}

std::optional<Duration> read_playout(Reader& in) {
    const uint8_t given = in.u8();
    if (given > 1) in.fail(Errc::Malformed);
    if (given != 1) return std::nullopt;
    const uint64_t us = in.varint();
    if (us > static_cast<uint64_t>(kMaxPlayout.count())) in.fail(Errc::Malformed);
    return Duration{static_cast<Duration::rep>(us)};
}

template <typename Message>
Result<ControlMessage> read_playout_message(Reader& in) {
    const std::optional<Duration> value = read_playout(in);
    if (!in.ok()) return fail(in.error());
    return ControlMessage{Message{value}};
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
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, PlayoutNeed>) {
                assert(!m.latency || (*m.latency >= Duration::zero() && *m.latency <= kMaxPlayout));
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, PlayoutDelay>) {
                assert(!m.delay || (*m.delay >= Duration::zero() && *m.delay <= kMaxPlayout));
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, Forward>) {
                assert(m.viewers.size() <= kMaxForwarded);
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, RelayedPath>) {
                assert(m.signal.queue_delay >= Duration::zero() &&
                       m.signal.rtt >= Duration::zero() && m.signal.queue_rise >= Duration::zero());
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, Chat>) {
                assert(is_valid(ChatMessage{.sent = m.sent, .text = m.text}));
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, ChatTold>) {
                assert(is_valid(m.message));
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
        case MessageKind::ClockProbe: return read_clock_probe(in);
        case MessageKind::ClockEcho: return read_clock_echo(in);
        case MessageKind::PlayoutNeed: return read_playout_message<PlayoutNeed>(in);
        case MessageKind::PlayoutDelay: return read_playout_message<PlayoutDelay>(in);
        case MessageKind::Forward: return read_forward(in);
        case MessageKind::RelayedPath: return read_relayed_path(in);
        case MessageKind::Uplink: return read_uplink(in);
        case MessageKind::CannotReach: return read_cannot_reach(in);
        case MessageKind::Forwarder: return ControlMessage{Forwarder{}};
        case MessageKind::FrameKey: return read_frame_key(in);
        case MessageKind::Chat: return read_chat(in);
        case MessageKind::ChatAsk: return ControlMessage{ChatAsk{}};
        case MessageKind::ChatTold: return read_chat_told(in);
        case MessageKind::Frame:
        case MessageKind::RelayedFrame: break;
    }
    return fail(Errc::UnknownKind);  // not a control message
}

}  // namespace ratcast::wire
