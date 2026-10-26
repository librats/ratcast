#include "ratcast/client/call.h"

#include <algorithm>
#include <type_traits>
#include <utility>

#include "ratcast/session/media_service.h"

#include "librats/subsystems/dht_discovery.h"
#include "librats/subsystems/hole_punch.h"
#include "librats/subsystems/peer_exchange.h"
#include "librats/subsystems/port_mapping_service.h"
#include "librats/subsystems/relay.h"
#include "librats/subsystems/rendezvous.h"
#include "librats/util/network_utils.h"

namespace ratcast {
namespace {

/// How often what this side needs of the watched's share is looked at, and
/// what its viewers all play at followed.
constexpr Duration kTogetherEvery = std::chrono::milliseconds(100);

/// The DHT key a discoverable call is found under: its own peer id, where it
/// and whoever looks for it meet.
std::string discovery_key(const PeerId& peer) { return "ratcast/stream/" + peer.to_hex(); }

const PeerId* dialed_peer(const CallConfig& config) {
    return config.dial ? std::get_if<PeerId>(&*config.dial) : nullptr;
}

/// Whether the first of what `peer` offers plays, with nothing chosen: it is
/// the peer dialed, or none was dialed by peer id.
bool first_of(const CallConfig& config, const PeerId& peer) {
    const PeerId* dialed = dialed_peer(config);
    return !dialed || peer == *dialed;
}

/// A call dialing by peer id listens, for the other side to dial back, and
/// a room's member, for those who join later.
bool listens(const CallConfig& config) {
    return config.listen || dialed_peer(config) || config.mesh;
}

librats::NodeConfig node_config(const CallConfig& config) {
    librats::NodeConfig node;
    node.listen_port = config.port;
    node.enable_listen = listens(config);
    node.bind_address = config.bind_address;
    node.data_dir = config.data_dir;
    node.protocol = config.protocol;
    node.enable_network_monitor = config.watch_network;
    // Paths are sampled every 100 ms, as MediaService's rate control asks.
    return node;
}

/// An address the other side may reach: not loopback, not link-local (which
/// would need an interface to go with it).
bool invitable(std::string_view address) {
    return !address.starts_with("127.") && address != "::1" && !address.starts_with("169.254.") &&
           !address.starts_with("fe80:") && !address.starts_with("FE80:") &&
           address.find('%') == std::string_view::npos;
}

bool is_ipv6(std::string_view address) { return address.find(':') != std::string_view::npos; }

}  // namespace

// ── Published streams ───────────────────────────────────────────────────────

struct Call::Published {
    explicit Published(std::string label) : label(std::move(label)) {}
    virtual ~Published() = default;
    virtual void stop() = 0;
    virtual PublisherStats stats() const = 0;
    virtual void request_keyframe() {}
    virtual void set_bitrate(uint32_t) {}

    const std::string label;
};

/// A source and its publisher: stopped (or destroyed) before the source.
template <typename Source, typename Publisher>
struct Call::PublishedAs final : Published {
    static constexpr bool kVideo = std::is_same_v<Publisher, VideoPublisher>;

    template <typename Config>
    PublishedAs(std::unique_ptr<Source> source, MediaSession& session, Config config)
        : Published(config.label),
          source(std::move(source)),
          publisher(session, std::move(config)) {}

    void stop() override { publisher.stop(); }
    PublisherStats stats() const override { return publisher.stats(); }
    void request_keyframe() override {
        if constexpr (kVideo) publisher.request_keyframe();
    }
    void set_bitrate(uint32_t bits_per_second) override {
        if constexpr (kVideo) publisher.set_bitrate(bits_per_second);
    }

    std::unique_ptr<Source> source;
    Publisher publisher;
};

// ── The call ────────────────────────────────────────────────────────────────

template <typename Handler, typename... Args>
void Call::notify(const Handler& handler, const Args&... args) const {
    if (handler && !closing_) handler(args...);
}

Result<std::unique_ptr<Call>> Call::start(CallConfig config, CallHandlers handlers) {
    std::unique_ptr<Call> call(new Call(std::move(config), std::move(handlers)));
    if (!call->node_.start()) return fail(Errc::Unavailable);
    if (call->config_.dial) {
        if (const auto* endpoint = std::get_if<librats::HostEndpoint>(&*call->config_.dial)) {
            call->node_.connect(endpoint->host, endpoint->port);
        }
    }
    return call;
}

Call::Call(CallConfig config, CallHandlers handlers)
    : config_(std::move(config)),
      handlers_(std::move(handlers)),
      node_(node_config(config_)),
      sync_(SteadyClock::instance(), config_.playback.sync) {
    if (config_.playback.sound) mixer_.emplace(*config_.playback.sound);
    // What it sends is sealed end to end, its key given to members alone: a
    // server that carries or forwards it reads nothing.
    media_ = node_.add_subsystem(std::make_unique<MediaService>(
        session_handlers(),
        SessionConfig{.capture_clock = config_.capture_clock,
                      .relays = config_.relays,
                      .entitled = [this](const PeerId& peer) { return !is_server(peer); }}));
    // One key to meet under: the peer dialed, or this call's own — in the
    // DHT, where whoever is met dials the port the router forwards, if it
    // does (a NAT then needs no punch), and at servers, which tell each side
    // where the other's NAT maps it and carry what no punch gets through.
    // A room's member joined by address meets nobody so, but knows a server
    // its peers name for one.
    const PeerId* dialed = dialed_peer(config_);
    const bool meets = dialed || config_.discoverable;
    librats::Rendezvous::Config meeting;
    if (meets) {
        const std::string key = discovery_key(dialed ? *dialed : node_.local_id());
        if (config_.dht) {
            node_.add_subsystem(std::make_unique<librats::PortMappingService>());
            librats::DhtDiscovery::Config discovery;
            discovery.discovery_key = key;
            discovery.data_dir = config_.data_dir;
            discovery.search_interval = std::chrono::seconds(10);
            node_.add_subsystem(std::make_unique<librats::DhtDiscovery>(std::move(discovery)));
        }
        meeting.keys = {key};
    }
    if (meets || config_.mesh) {
        // A room's members met by address are carried by a server they all
        // reach too, where no punch gets through.
        meeting.servers = config_.servers;
        rendezvous_ = node_.add_subsystem(std::make_unique<librats::Rendezvous>(meeting));
        // A server learnt of from a member was taken for a member as it came.
        rendezvous_->on_server_found([this](const PeerId& peer) { notify(handlers_.left, peer); });
        node_.add_subsystem(std::make_unique<librats::HolePunch>());
        node_.add_subsystem(std::make_unique<librats::Relay>());
    }
    if (config_.mesh) {
        librats::PeerExchange::Config exchange;
        exchange.introduce_requesters = true;
        node_.add_subsystem(std::make_unique<librats::PeerExchange>(exchange));
    }
    node_.on_peer_connected([this](const librats::Peer& peer) {
        if (!is_server(peer.id())) notify(handlers_.joined, peer.id());
    });
    node_.on_peer_disconnected([this](const PeerId& peer, librats::CloseReason) {
        // Still known: the subsystems' handlers, attached as the node
        // starts, run after this one.
        if (!is_server(peer)) notify(handlers_.left, peer);
    });
}

Call::~Call() {
    closing_ = true;
    std::map<StreamId, std::unique_ptr<Published>> published;
    {
        std::lock_guard lock(publishing_);
        published.swap(published_);
    }
    for (auto& [stream, publisher] : published) publisher->stop();
    published.clear();
    {
        // Players stop outside the lock: their threads may be in a handler
        // that asks for stats.
        Playing<VideoPlayer> video;
        Playing<AudioPlayer> sound;
        std::map<PeerId, Member> members;
        Playing<DataPlayer> feed;
        std::unique_ptr<DataPlayer> ended_feed;
        std::lock_guard lock(playing_);
        video = std::move(video_);
        sound = std::move(sound_);
        members = std::move(members_);
        feed = std::move(feed_);
        ended_feed = std::move(ended_feed_);
    }
    node_.stop();
}

const PeerId& Call::id() const noexcept { return node_.local_id(); }

uint16_t Call::port() const noexcept { return listens(config_) ? node_.listen_port() : 0; }

std::vector<std::string> Call::invites() const {
    std::vector<std::string> links;
    // A room joined by peer id is found under that id: its members announce
    // themselves there too.
    if (const PeerId* dialed = dialed_peer(config_); dialed && config_.mesh) {
        links.push_back(to_string(RatsUrl{*dialed}));
    } else if (config_.discoverable && !dialed) {
        links.push_back(to_string(RatsUrl{id()}));
    }
    const uint16_t listening = port();
    if (listening == 0) return links;

    const std::string& bound = config_.bind_address;
    std::vector<std::string> hosts;
    if (!bound.empty() && bound != "::" && bound != "0.0.0.0") {
        hosts.push_back(bound);
    } else {
        for (std::string& address : librats::network_utils::get_local_interface_addresses()) {
            if (!invitable(address) || (bound == "0.0.0.0" && is_ipv6(address)) ||
                std::ranges::find(hosts, address) != hosts.end()) {
                continue;
            }
            hosts.push_back(std::move(address));
        }
        std::ranges::stable_partition(hosts, [](const std::string& h) { return !is_ipv6(h); });
    }
    for (std::string& host : hosts) {
        links.push_back(to_string(RatsUrl{librats::HostEndpoint{std::move(host), listening}}));
    }
    return links;
}

std::vector<PeerId> Call::peers() const {
    std::vector<PeerId> peers = node_.connected_peers();
    std::erase_if(peers, [this](const PeerId& peer) { return is_server(peer); });
    return peers;
}

std::vector<PeerId> Call::servers() const {
    return rendezvous_ ? rendezvous_->servers() : std::vector<PeerId>{};
}

bool Call::is_server(const PeerId& peer) const {
    return rendezvous_ && rendezvous_->is_server(peer);
}

Result<void> Call::set_presence(const Presence& presence) {
    return session().set_presence(presence);
}

std::optional<Presence> Call::presence(const PeerId& peer) const {
    return media_->session().presence(peer);
}

std::optional<librats::PathStats> Call::path(const PeerId& peer) const {
    return node_.path_stats(peer);
}

bool Call::relayed(const PeerId& peer) const {
    const std::vector<librats::PeerInfo> peers = node_.peers();
    const auto info = std::ranges::find(peers, peer, &librats::PeerInfo::id);
    return info != peers.end() && info->transport == librats::TransportKind::Relay;
}

MediaSession& Call::session() noexcept { return media_->session(); }

SessionHandlers Call::session_handlers() {
    return SessionHandlers{
        .stream_announced = [this](const PeerId& peer,
                                   const StreamDescription& d) { announced(peer, d); },
        .stream_withdrawn = [this](const PeerId& peer, StreamId s) { withdrawn(peer, s); },
        .frame_received = [this](const PeerId& peer,
                                 EncodedFrame frame) { received(peer, std::move(frame)); },
        .keyframe_needed =
            [this](StreamId stream) {
                std::lock_guard lock(publishing_);
                if (const auto it = published_.find(stream); it != published_.end()) {
                    it->second->request_keyframe();
                }
            },
        .bitrate_changed =
            [this](StreamId stream, uint32_t bits_per_second) {
                std::lock_guard lock(publishing_);
                if (const auto it = published_.find(stream); it != published_.end()) {
                    it->second->set_bitrate(bits_per_second);
                }
            },
        .presence_changed =
            [this](const PeerId& peer, const Presence& presence) {
                notify(handlers_.presence, peer, presence);
            },
    };
}

// ── What this side sends ────────────────────────────────────────────────────

Result<VideoPublisher*> Call::publish(std::unique_ptr<VideoSource> source,
                                      VideoPublisherConfig config) {
    return add<VideoSource, VideoPublisher>(std::move(source), std::move(config));
}

Result<AudioPublisher*> Call::publish(std::unique_ptr<AudioSource> source,
                                      AudioPublisherConfig config) {
    return add<AudioSource, AudioPublisher>(std::move(source), std::move(config));
}

Result<DataPublisher*> Call::publish(std::unique_ptr<TsInput> input, DataPublisherConfig config) {
    return add<TsInput, DataPublisher>(std::move(input), std::move(config));
}

template <typename Source, typename Publisher, typename Config>
Result<Publisher*> Call::add(std::unique_ptr<Source> source, Config config) {
    if (!source) return fail(Errc::InvalidArgument);
    const StreamId stream = config.stream;
    auto published = std::make_unique<PublishedAs<Source, Publisher>>(std::move(source), session(),
                                                                      std::move(config));
    Publisher& publisher = published->publisher;
    Source& input = *published->source;
    {
        std::lock_guard lock(publishing_);
        if (!published_.try_emplace(stream, std::move(published)).second) {
            return fail(Errc::InvalidArgument);
        }
    }
    const Result<void> started = publisher.start(input, [this, stream](Result<void> outcome) {
        notify(handlers_.source_ended, stream, outcome);
    });
    if (!started) {
        unpublish(stream);
        return fail(started.error());
    }
    return &publisher;
}

void Call::unpublish(StreamId stream) {
    std::unique_ptr<Published> published;
    {
        std::lock_guard lock(publishing_);
        const auto it = published_.find(stream);
        if (it == published_.end()) return;
        published = std::move(it->second);
        published_.erase(it);
    }
    published->stop();
}

std::vector<SentStreamStats> Call::sent_stats() const {
    std::vector<SentStreamStats> sent;
    std::lock_guard lock(publishing_);
    sent.reserve(published_.size());
    for (const auto& [stream, published] : published_) {
        sent.push_back({stream, published->label, published->stats(),
                        media_->session().outgoing_stats(stream).value_or(OutgoingStreamStats{})});
    }
    return sent;
}

// ── What plays ──────────────────────────────────────────────────────────────

template <typename Player>
bool Call::plays(const Playing<Player>& slot, const PeerId& peer, StreamId stream) {
    return slot && slot.player->peer() == peer && slot.description.id == stream;
}

template <typename Player>
void Call::play(Playing<Player>& slot, const PeerId& peer, const StreamDescription& description,
                std::vector<std::function<void()>>& events) {
    const CallPlayback& playback = config_.playback;
    MediaSession& media = session();
    std::unique_ptr<AudioMixer::Input> output;
    std::unique_ptr<Player> player;
    if constexpr (std::is_same_v<Player, VideoPlayer>) {
        // A camera keeps its own time; the other pictures go with the sound.
        const bool camera = description.label == kCameraLabel;
        VideoPlayer::Sink sink = camera ? playback.cameras(peer) : playback.pictures;
        if (!sink) return;
        VideoPlayerConfig video = playback.video;
        video.sync = camera ? nullptr : &sync_;
        player = std::make_unique<VideoPlayer>(media, peer, description, std::move(sink), video);
    } else if constexpr (std::is_same_v<Player, AudioPlayer>) {
        // A voice keeps its own time; the other sound goes with the pictures.
        const bool voice = description.label == kVoiceLabel;
        AudioPlayer::Observer observer;
        if (playback.heard) {
            observer = [&heard = playback.heard, peer](Timestamp capture_time,
                                                       Timestamp play_time) {
                heard(peer, capture_time, play_time);
            };
        }
        output = mixer_->input();
        if (!voice) {
            output->set_volume(share_volume_);
        } else if (const auto set = volumes_.find(peer); set != volumes_.end()) {
            output->set_volume(set->second);
        }
        player = std::make_unique<AudioPlayer>(media, peer, description, *output,
                                               AudioPlayerConfig{.sync = voice ? nullptr : &sync_},
                                               std::move(observer));
    } else {
        player = std::make_unique<DataPlayer>(media, peer, description, *playback.feed,
                                              playback.feed_config);
    }
    Result<void> started = player->start();
    if (started) slot = {std::move(output), std::move(player), description};
    events.push_back([this, peer, description, started] {
        notify(handlers_.playing, peer, description, started);
    });
}

void Call::announced(const PeerId& peer, const StreamDescription& description) {
    std::vector<std::function<void()>> events;
    {
        std::lock_guard lock(playing_);
        if (closing_) return;
        // A change to pictures that play: the player follows it.
        const auto member = members_.find(peer);
        for (Playing<VideoPlayer>* slot :
             {&video_, member != members_.end() ? &member->second.camera : nullptr}) {
            if (!slot || !plays(*slot, peer, description.id)) continue;
            slot->player->update(description);
            slot->description = description;
            return;
        }
        offer(peer, description, events);
    }
    for (const auto& event : events) event();
}

void Call::offer(const PeerId& peer, const StreamDescription& description,
                 std::vector<std::function<void()>>& events) {
    const CallPlayback& playback = config_.playback;
    switch (description.kind()) {
        case MediaKind::Video:
            if (description.label == kCameraLabel) {
                if (!playback.cameras) return;
                Member& member = members_[peer];
                if (!member.camera) play(member.camera, peer, description, events);
                if (member.empty()) members_.erase(peer);
            } else if (playback.pictures && !video_ && watches(peer)) {
                play(video_, peer, description, events);
            }
            return;
        case MediaKind::Audio:
            if (!playback.sound) return;
            if (description.label == kVoiceLabel) {
                Member& member = members_[peer];
                if (!member.voice) play(member.voice, peer, description, events);
                if (member.empty()) members_.erase(peer);
            } else if (!sound_ && watches(peer)) {
                play(sound_, peer, description, events);
            }
            return;
        case MediaKind::Data:
            if (playback.feed && !feed_ && first_of(config_, peer)) {
                play(feed_, peer, description, events);
            }
            return;
    }
}

bool Call::watches(const PeerId& peer) const {
    if (chosen_) return watched_ == peer;
    // The first offered: the pictures and the sound of one peer.
    if (video_) return video_.player->peer() == peer;
    if (sound_) return sound_.player->peer() == peer;
    return first_of(config_, peer);
}

void Call::watch(std::optional<PeerId> peer) {
    // Stopped outside the lock, as in the destructor.
    Playing<VideoPlayer> video;
    Playing<AudioPlayer> sound;
    std::vector<std::pair<PeerId, StreamDescription>> ended;
    {
        std::lock_guard lock(playing_);
        if (closing_) return;
        chosen_ = true;
        watched_ = std::move(peer);
        if (video_ && !watches(video_.player->peer())) {
            ended.emplace_back(video_.player->peer(), video_.description);
            video = std::move(video_);
        }
        if (sound_ && !watches(sound_.player->peer())) {
            ended.emplace_back(sound_.player->peer(), sound_.description);
            sound = std::move(sound_);
        }
    }
    video.reset();
    sound.reset();
    for (const auto& [from, description] : ended) {
        notify(handlers_.ended, from, description);
        leave_together(from);
    }
    if (!ended.empty()) sync_.forget();
    play_watched();
}

void Call::play_watched() {
    std::vector<std::function<void()>> events;
    {
        std::lock_guard lock(playing_);
        if (closing_) return;
        for (const OfferedStream& offered : session().offered()) {
            const StreamDescription& description = offered.description;
            // Voices, cameras and a feed play whoever is watched.
            if (description.kind() == MediaKind::Data || description.label == kVoiceLabel ||
                description.label == kCameraLabel) {
                continue;
            }
            offer(offered.peer, description, events);
        }
    }
    for (const auto& event : events) event();
}

void Call::withdrawn(const PeerId& peer, StreamId stream) {
    // Stopped outside the lock, as in the destructor.
    Playing<VideoPlayer> video;
    Playing<AudioPlayer> sound;
    std::unique_ptr<DataPlayer> replaced;  ///< the feed that ended before, done or not
    std::optional<StreamDescription> ended;
    bool watched = false;  ///< what ended was the watched pictures or sound
    {
        std::lock_guard lock(playing_);
        if (plays(video_, peer, stream)) {
            ended = video_.description;
            video = std::move(video_);
            watched = true;
        } else if (plays(sound_, peer, stream)) {
            ended = sound_.description;
            sound = std::move(sound_);
            watched = true;
        } else if (plays(feed_, peer, stream)) {
            ended = feed_.description;
            // What it holds is still to be written out, each at its time.
            feed_.player->finish();
            replaced = std::exchange(ended_feed_, std::move(feed_.player));
        } else if (const auto it = members_.find(peer); it != members_.end()) {
            Member& member = it->second;
            if (plays(member.camera, peer, stream)) {
                ended = member.camera.description;
                video = std::move(member.camera);
            } else if (plays(member.voice, peer, stream)) {
                ended = member.voice.description;
                sound = std::move(member.voice);
            }
            if (member.empty()) members_.erase(it);
        }
    }
    video.reset();
    sound.reset();
    replaced.reset();
    if (ended) notify(handlers_.ended, peer, *ended);
    if (watched) {
        leave_together(peer);
        sync_.forget();  // for whoever's next
        play_watched();
    }
}

void Call::received(const PeerId& peer, EncodedFrame frame) {
    std::lock_guard lock(playing_);
    const StreamId stream = frame.info.stream;
    if (plays(video_, peer, stream)) {
        video_.player->push(std::move(frame));
        keep_together(peer);
    } else if (plays(sound_, peer, stream)) {
        sound_.player->push(std::move(frame));
        keep_together(peer);
    } else if (plays(feed_, peer, stream)) {
        feed_.player->push(std::move(frame));
    } else if (const auto it = members_.find(peer); it != members_.end()) {
        Member& member = it->second;
        if (plays(member.voice, peer, stream)) {
            member.voice.player->push(std::move(frame));
        } else if (plays(member.camera, peer, stream)) {
            member.camera.player->push(std::move(frame));
        }
    }
}

void Call::keep_together(const PeerId& peer) {
    if (!config_.playback.together) return;
    const Timestamp now = SteadyClock::instance().now();
    if (now - together_at_ < kTogetherEvery) return;
    together_at_ = now;
    MediaSession& media = session();
    if (const std::optional<Duration> need = sync_.need()) media.set_playout_need(peer, *need);
    sync_.play_together(media.common_playout(peer));
}

void Call::leave_together(const PeerId& peer) {
    if (config_.playback.together) session().set_playout_need(peer, std::nullopt);
}

std::optional<ReceivedVideoStats> Call::video_stats() const {
    std::lock_guard lock(playing_);
    return stats_of(video_);
}

std::map<PeerId, ReceivedVideoStats> Call::camera_stats() const {
    std::map<PeerId, ReceivedVideoStats> cameras;
    std::lock_guard lock(playing_);
    for (const auto& [peer, member] : members_) {
        if (std::optional<ReceivedVideoStats> camera = stats_of(member.camera)) {
            cameras.emplace(peer, std::move(*camera));
        }
    }
    return cameras;
}

std::optional<ReceivedVideoStats> Call::stats_of(const Playing<VideoPlayer>& slot) const {
    if (!slot) return std::nullopt;
    const std::optional<IncomingStreamStats> stream =
        media_->session().incoming_stats(slot.player->peer(), slot.player->stream());
    if (!stream) return std::nullopt;
    return ReceivedVideoStats{slot.player->peer(), slot.player->stats(), *stream};
}

std::optional<AudioPlayerStats> Call::sound_stats() const {
    std::lock_guard lock(playing_);
    if (!sound_) return std::nullopt;
    return sound_.player->stats();
}

void Call::set_volume(const PeerId& peer, float volume) {
    volume = volume > 0.0f ? volume : 0.0f;  // as the mixer takes it, NaN 0
    std::lock_guard lock(playing_);
    if (volume == 1.0f) {
        volumes_.erase(peer);
    } else {
        volumes_[peer] = volume;
    }
    const auto member = members_.find(peer);
    if (member != members_.end() && member->second.voice) {
        member->second.voice.output->set_volume(volume);
    }
}

float Call::volume(const PeerId& peer) const {
    std::lock_guard lock(playing_);
    const auto set = volumes_.find(peer);
    return set != volumes_.end() ? set->second : 1.0f;
}

void Call::set_share_volume(float volume) {
    volume = volume > 0.0f ? volume : 0.0f;  // as the mixer takes it, NaN 0
    std::lock_guard lock(playing_);
    share_volume_ = volume;
    if (sound_) sound_.output->set_volume(volume);
}

float Call::share_volume() const {
    std::lock_guard lock(playing_);
    return share_volume_;
}

std::vector<PeerId> Call::speaking(Duration hold) const {
    const Timestamp now = SteadyClock::instance().now();
    std::vector<PeerId> speaking;
    std::lock_guard lock(playing_);
    for (const auto& [peer, member] : members_) {
        if (!member.voice) continue;
        const std::optional<Timestamp> heard = member.voice.player->stats().heard;
        if (heard && now - *heard < hold) speaking.push_back(peer);
    }
    return speaking;
}

std::map<PeerId, AudioPlayerStats> Call::voice_stats() const {
    std::map<PeerId, AudioPlayerStats> voices;
    std::lock_guard lock(playing_);
    for (const auto& [peer, member] : members_) {
        if (member.voice) voices.emplace(peer, member.voice.player->stats());
    }
    return voices;
}

std::optional<DataPlayerStats> Call::feed_stats() const {
    std::lock_guard lock(playing_);
    const DataPlayer* feed = feed_ ? feed_.player.get() : ended_feed_.get();
    if (!feed) return std::nullopt;
    return feed->stats();
}

PlayoutClockStats Call::sync_stats() const { return sync_.stats(); }

}  // namespace ratcast
