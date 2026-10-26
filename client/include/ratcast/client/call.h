#pragma once

/**
 * @file call.h
 * @brief One side of a call: its node, what it sends, what it plays.
 *
 * A Call is everything between this side's sources and devices and the other
 * side: a librats node (its identity kept across runs if asked) with a
 * MediaService on it, the publishers of what this side sends, and the players
 * of what the other side sends.
 *
 * What is published, the call owns: each source with its publisher, whose
 * encoder hears the keyframes receivers ask for and the bitrate rate control
 * aims at. Whoever connects may subscribe to it.
 *
 * What is played, each where the playback config says, is everyone's voice
 * and camera, and of the one watched the pictures and the sound with them.
 * A voice is a sound stream labelled kVoiceLabel, a camera a video stream
 * labelled kCameraLabel: each peer's play in their own time, the voices all
 * mixed into the one output (AudioMixer). The pictures and the sound are
 * those of the peer chosen with watch() — or, until then, of the first to
 * offer either, the peer dialed if it was dialed by peer id — and play in step
 * (PlayoutClock), the sound mixed with the voices, and in step with the
 * sender's other viewers: each frame everywhere at once. When they end, the next
 * the watched offers play. A feed is the first offered, by the peer dialed if
 * it was dialed by peer id; when it ends, the next announced. Only what plays
 * is received: a screen nobody here watches costs nothing.
 *
 * The other side gets here by an invite (invites()): a rats:// link with this
 * node's peer id, looked for through the DHT if the call is discoverable, or
 * with one of the host's addresses and the port it listens on.
 *
 * A call met by peer id meets at servers as well, if it knows one
 * (CallConfig::servers, or a member's naming one): public nodes where both
 * sides learn where the other's NAT maps it, dial at once, and are carried
 * where no dial gets through (ratcast-server, librats' Rendezvous and Relay);
 * a room's members, however they joined, are carried by one they all reach.
 * A server is no member: never among peers(), nor joined, nor given the key
 * this side's frames are sealed under end to end — what it carries or
 * forwards, it cannot read. A server tells of the others it knows, which the
 * call keeps in its data directory (known_servers(), KnownServers); of those,
 * a call met by peer id meets at a few chosen by its key, the same at every
 * member (CallConfig::chosen_servers), and at the ones the link named
 * (RatsLink's via, given as CallConfig::servers) — which invites() name in
 * turn.
 *
 * A call may be a room (CallConfig::mesh): everyone with everyone, whoever
 * joins by whichever member's link. Each peer connected is asked whom it has,
 * and they are dialed (librats' PeerExchange, a hole punched through a peer
 * both have where a dial fails); each that asks is introduced to the rest, so
 * that two joining at once meet too. A member listens, for those who come
 * later.
 *
 * What members write (say()) goes to every member, and the last of it is
 * told to each that comes, or comes back (chat.h): a server is told nothing.
 *
 * Threads: every method may be called from any thread. Handlers run on the
 * network's, a publisher's or a player's threads, never under the call's
 * locks, and must not block or destroy the call; source_ended must not
 * unpublish the stream that ended. None runs once the destructor has begun.
 */

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "ratcast/client/audio_player.h"
#include "ratcast/client/audio_publisher.h"
#include "ratcast/client/data_player.h"
#include "ratcast/client/data_publisher.h"
#include "ratcast/client/known_servers.h"
#include "ratcast/client/playout_clock.h"
#include "ratcast/client/rats_url.h"
#include "ratcast/client/video_player.h"
#include "ratcast/client/video_publisher.h"
#include "ratcast/core/result.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/audio_mixer.h"
#include "ratcast/engine/ts_io.h"
#include "ratcast/ratcast.h"
#include "ratcast/session/chat.h"
#include "ratcast/session/media_session.h"

#include "librats/node/node.h"

namespace librats {
class Rendezvous;
}  // namespace librats

namespace ratcast {

class MediaService;

/// The label of a sound stream that is someone's voice: played as soon as it
/// can be, not held to pictures.
inline constexpr std::string_view kVoiceLabel = "voice";
/// The label of a video stream that is someone's camera: played beside the
/// other pictures, in its own time.
inline constexpr std::string_view kCameraLabel = "camera";

/// What plays, and where. Each kind left without a destination is not
/// received at all.
struct CallPlayback {
    /// Pictures, on the video player's thread.
    VideoPlayer::Sink pictures = {};
    VideoPlayerConfig video = {};  ///< its `sync` is the call's own
    /// Where `peer`'s camera's pictures go, asked as it starts (null: not
    /// played), then called on its player's thread; `video`'s config. Asked
    /// under the call's lock: it must not call back into the call.
    std::function<VideoPlayer::Sink(const PeerId& peer)> cameras = {};
    /// The most cameras played at once: those of the last to speak, the
    /// rest not received (last-N, camera_choice.h), looked at a few times a
    /// second; 0, every one offered.
    size_t most_cameras = 0;
    /// The device the sound and the voices play into; must outlive the call.
    AudioOutput* sound = nullptr;
    /// Each frame of sound from `peer` — a voice, or the watched's — captured
    /// at `capture_time` on its clock, heard at `play_time` on the output's.
    /// Called on the output's thread.
    std::function<void(const PeerId& peer, Timestamp capture_time, Timestamp play_time)> heard = {};
    /// Pictures and sound of one sender in step; apart, each keeps its own
    /// time (their distance is measured all the same).
    bool sync = true;
    /// The pictures and sound watched played when the sender's other viewers
    /// play them, each frame at the same moment everywhere
    /// (MediaSession::set_playout_need()); apart, as soon as this side can.
    bool together = true;
    /// Where a feed is written out; must outlive the call.
    TsOutput* feed = nullptr;
    DataPlayerConfig feed_config = {};
};

struct CallConfig {
    /// The port the other side dials; 0: any.
    uint16_t port = 0;
    /// A call that only dials an address need not listen; one dialing by peer
    /// id listens all the same, for the other side to dial back.
    bool listen = true;
    /// The interface to listen on: "" for every one, IPv6 and IPv4.
    std::string bind_address = {};
    /// Where the node keeps its identity — its peer id — and the DHT its
    /// table, across runs; "": a new identity each run.
    std::string data_dir = {};
    /// Findable by peer id through the DHT (invites() lists rats://PEER-ID).
    /// It takes a minute or two to converge on the open internet. A call that
    /// meets through the DHT (this, or dialing a peer id) asks the router to
    /// forward its port (UPnP, NAT-PMP), and both sides dial each other again
    /// while not connected, which gets through two NATs that keep a port to
    /// every peer alike.
    bool discoverable = false;
    /// Who to call: an address, dialed at once, or a discoverable node's peer
    /// id, looked for through the DHT. None: wait to be called.
    std::optional<RatsUrl> dial = std::nullopt;
    /// Servers a call met by peer id (this discoverable, or dialing one) meets
    /// at too, and a room's member at all, dialed and kept while the call
    /// lasts: "host:port".
    std::vector<librats::HostEndpoint> servers = {};
    /// A call met by peer id is looked for, and announced, through the DHT as
    /// well as at the servers; false: at the servers alone.
    bool dht = true;
    /// A call met by peer id meets, besides `servers`, at so many of the
    /// servers it knows (known_servers()): the same ones at every member,
    /// chosen by the key it is met under (librats' Rendezvous::choose()), so
    /// that members who named no server meet at one all the same. 0: none.
    size_t chosen_servers = 0;
    /// The servers known from the start (KnownServers::built_in(): the
    /// build's), and where those servers tell of are kept from one run to the
    /// next — "": `servers` in the data directory, if there is one.
    std::vector<librats::HostEndpoint> built_in_servers = KnownServers::built_in();
    std::string known_servers_file = {};
    /// Nodes of another protocol never connect: another version, or a test.
    std::string protocol = RATCAST_PROTOCOL;
    /// Follow the host's network changes (interfaces, addresses, sleep).
    bool watch_network = true;
    /// A room: meet everyone the peers met (see above). Listens whatever
    /// `listen` says.
    bool mesh = false;
    /// The clock this side's sources capture on, if not the steady clock (the
    /// tools' stamped sources: the time of day). The other side measures its
    /// clock against it to play what this side shares in step. Must outlive
    /// the call.
    Clock* capture_clock = nullptr;
    /// What this side shares goes through relays — viewers that pass it on —
    /// while the copies through its uplink are short (SessionConfig::relays).
    bool relays = true;
    /// A peer whose link stays silent this long is lost (librats' UDP idle
    /// timeout); zero: librats' own, 45 s. A link silent for less is kept,
    /// and plays on when it hears again.
    Duration lost_after = Duration::zero();
    /// A peer lost — its link gone silent or broken, closed by neither side —
    /// is dialed again at where it was last known (and dials this side, which
    /// it lost too) for this long, away() meanwhile, then left; zero: left at
    /// once. A peer that hangs up is left at once.
    Duration redial_for = std::chrono::minutes(2);
    CallPlayback playback = {};
};

struct CallHandlers {
    /// A peer connected — or came back after it was lost — or left: hung up,
    /// or was lost and did not come back in time (CallConfig::redial_for).
    std::function<void(const PeerId& peer)> joined = {};
    std::function<void(const PeerId& peer)> left = {};
    /// A peer's link was lost and it is being dialed again: away() until it
    /// joins again or leaves.
    std::function<void(const PeerId& peer)> lost = {};
    /// A stream of `peer`'s started playing, or failed to (the outcome).
    std::function<void(const PeerId& peer, const StreamDescription& stream, Result<void> outcome)>
        playing = {};
    /// A stream that played ended: `peer` withdrew it, or left, or another
    /// is watched — or, a camera, made room for one whose member spoke later
    /// (CallPlayback::most_cameras).
    std::function<void(const PeerId& peer, const StreamDescription& stream)> ended = {};
    /// A peer said who it is, or that something of it changed (Presence).
    std::function<void(const PeerId& peer, const Presence& presence)> presence = {};
    /// A message new here: a member said it, or one told what was said before
    /// this side came or while it was away. Not what say() said.
    std::function<void(const ChatMessage& message)> said = {};
    /// A published stream stopped by itself: its source ran out (success),
    /// or the source or its encoder failed. It stays published until
    /// unpublish().
    std::function<void(StreamId stream, Result<void> outcome)> source_ended = {};
};

/// What a publisher counts, whichever kind it is.
using PublisherStats = std::variant<VideoPublisherStats, AudioPublisherStats, DataPublisherStats>;

/// A stream this side sends: its publisher's view and its stream's.
struct SentStreamStats {
    StreamId stream = 0;
    std::string label;
    PublisherStats publisher;
    OutgoingStreamStats session;
};

/// The video that plays: its player's view and its stream's.
struct ReceivedVideoStats {
    PeerId peer;  ///< whose
    VideoPlayerStats player;
    IncomingStreamStats stream;
};

class Call {
public:
    /// Start the node and, if the config says whom, call them. Fails with
    /// Unavailable when the node cannot start.
    static Result<std::unique_ptr<Call>> start(CallConfig config, CallHandlers handlers = {});

    /// Hang up: stop publishing and playing, leave every peer.
    ~Call();
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;

    // ── Who ─────────────────────────────────────────────────────────────────

    const PeerId& id() const noexcept;
    /// The port it listens on; 0 if it does not.
    uint16_t port() const noexcept;
    /// rats:// links the other side can call this one by: a peer id the DHT
    /// finds the call under — this call's, if it is discoverable, or, in a
    /// room, the one it dialed — with the servers of CallConfig::servers
    /// named after it (RatsLink's via), then each of the host's addresses (or
    /// the one it listens on) with the port. None for a call that does not
    /// listen.
    std::vector<std::string> invites() const;
    std::vector<PeerId> peers() const;
    /// The peers lost and dialed again (CallConfig::redial_for): none of
    /// peers(), members still.
    std::vector<PeerId> away() const;
    /// Whether `peer`'s link is cut: it is away, or connected with what it
    /// answers unanswered for `after` — gone quiet, maybe lost, maybe back in a
    /// moment (librats' PathStats::unanswered_since).
    bool interrupted(const PeerId& peer, Duration after = std::chrono::seconds(2)) const;
    /// The servers it is connected to (CallConfig::servers, or named by a
    /// peer): none of peers().
    std::vector<PeerId> servers() const;
    /// The servers this side knows of: the build's, and those servers told it
    /// of — in this call or, kept in the data directory, an earlier one.
    std::vector<librats::HostEndpoint> known_servers() const;
    /// Say who this side is — a name, muted, deafened — to every peer now and
    /// to each as it connects. Fails with InvalidArgument for a presence that
    /// is not valid.
    Result<void> set_presence(const Presence& presence);
    /// What `peer` last said of itself — while it is away, before it was
    /// lost; none before it said anything, or once it left.
    std::optional<Presence> presence(const PeerId& peer) const;
    /// The path to `peer`, sampled every 100 ms: round trip, bandwidth, loss.
    std::optional<librats::PathStats> path(const PeerId& peer) const;
    /// Whether that path goes through a relay — a server carrying the
    /// connection, no punch having got through — rather than straight.
    bool relayed(const PeerId& peer) const;

    // ── What is said ────────────────────────────────────────────────────────

    /// Say `text` to every member, under the name set_presence() gave, as of
    /// now by this host's clock: kept, and told to whoever comes later
    /// (MediaSession::say(); never to a server). Fails with InvalidArgument
    /// for a text no message has (is_valid()).
    Result<ChatMessage> say(std::string text);
    /// What this side knows was said, oldest first: the last 100 messages.
    std::vector<ChatMessage> chat() const;

    // ── What this side sends ────────────────────────────────────────────────

    /// Publish what `source` produces as the stream `config.stream`, until
    /// unpublish(); the publisher is the call's until then, for its stats.
    /// Fails with InvalidArgument for no source or a stream id in use, or
    /// with the source's own failure to start.
    Result<VideoPublisher*> publish(std::unique_ptr<VideoSource> source,
                                    VideoPublisherConfig config);
    Result<AudioPublisher*> publish(std::unique_ptr<AudioSource> source,
                                    AudioPublisherConfig config);
    Result<DataPublisher*> publish(std::unique_ptr<TsInput> input, DataPublisherConfig config);
    /// Stop the source and withdraw the stream.
    void unpublish(StreamId stream);
    /// Every stream published, by id.
    std::vector<SentStreamStats> sent_stats() const;

    // ── What plays ──────────────────────────────────────────────────────────

    /// Play `peer`'s pictures and the sound with them from now on, in place
    /// of whoever's played; nobody's with nullopt. Until this is called, the
    /// first offered play.
    void watch(std::optional<PeerId> peer);
    /// How loud `peer`'s voice plays on this side, and only here: 1 as it
    /// comes, 0 silent, 2 twice the amplitude (+6 dB). Kept while the call
    /// lasts, for a voice that plays later or again; a voice turned down is
    /// still heard as speaking (speaking(), `playback.heard`).
    void set_volume(const PeerId& peer, float volume);
    float volume(const PeerId& peer) const;
    /// How loud the sound with the pictures watched plays — a film's beside
    /// the voices — as set_volume() takes it; whoever is watched, for as long
    /// as the call lasts.
    void set_share_volume(float volume);
    float share_volume() const;

    /// The pictures and the sound with them, of the one watched.
    std::optional<ReceivedVideoStats> video_stats() const;
    std::optional<AudioPlayerStats> sound_stats() const;
    /// The peers whose voice was heard within `hold`: who is speaking, held a
    /// moment after they stop so that a UI showing it does not flicker.
    std::vector<PeerId> speaking(Duration hold = std::chrono::milliseconds(300)) const;
    /// The peers whose cameras play: every one offered, or the last to speak
    /// (CallPlayback::most_cameras).
    std::vector<PeerId> cameras() const;
    /// Each peer's camera and voice playing, by peer.
    std::map<PeerId, ReceivedVideoStats> camera_stats() const;
    std::map<PeerId, AudioPlayerStats> voice_stats() const;
    /// The feed playing, or the last one, writing out what it held or done.
    std::optional<DataPlayerStats> feed_stats() const;
    /// Sound and pictures: how far apart they played, and what each needs.
    PlayoutClockStats sync_stats() const;

    /// For stream stats (outgoing_stats(), incoming_stats()).
    MediaSession& session() noexcept;

private:
    struct Published;
    template <typename Source, typename Publisher>
    struct PublishedAs;
    template <typename Player>
    struct Playing {
        std::unique_ptr<AudioMixer::Input> output;  ///< a sound's
        std::unique_ptr<Player> player;             ///< after its output: stopped before it
        StreamDescription description;
        explicit operator bool() const noexcept { return player != nullptr; }
        /// The player first: it plays into the output.
        void reset() {
            player.reset();
            output.reset();
        }
    };

    /// What of one peer plays in its own time, whoever is watched.
    struct Member {
        Playing<AudioPlayer> voice;
        Playing<VideoPlayer> camera;
        bool empty() const noexcept { return !voice && !camera; }
    };

    Call(CallConfig config, CallHandlers handlers);

    SessionHandlers session_handlers();
    template <typename Source, typename Publisher, typename Config>
    Result<Publisher*> add(std::unique_ptr<Source> source, Config config);
    void announced(const PeerId& peer, const StreamDescription& description);
    void withdrawn(const PeerId& peer, StreamId stream);
    void received(const PeerId& peer, EncodedFrame frame);
    /// Play what the one watched offers where nothing plays: after what
    /// played ended, or another is watched.
    void play_watched();
    /// The cameras of the last to speak played, the rest let go
    /// (CallPlayback::most_cameras); a few times a second, on speakers_.
    void follow_speakers();
    // Under playing_; what came of it are events for after the lock.
    /// Play what `peer` offers if it is to play and nothing plays in its place.
    void offer(const PeerId& peer, const StreamDescription& description,
               std::vector<std::function<void()>>& events);
    /// Whether `peer`'s pictures and sound are to play.
    bool watches(const PeerId& peer) const;
    /// Start a player for `description` in `slot`, if it starts.
    template <typename Player>
    void play(Playing<Player>& slot, const PeerId& peer, const StreamDescription& description,
              std::vector<std::function<void()>>& events);
    template <typename Player>
    static bool plays(const Playing<Player>& slot, const PeerId& peer, StreamId stream);
    std::optional<ReceivedVideoStats> stats_of(const Playing<VideoPlayer>& slot) const;
    template <typename Handler, typename... Args>
    void notify(const Handler& handler, const Args&... args) const;
    /// Under playing_: what this side needs of `peer`'s share told now and
    /// then, and what its viewers all play at followed.
    void keep_together(const PeerId& peer);
    /// `peer`'s share is played in step no more.
    void leave_together(const PeerId& peer);
    /// A server, not a member.
    bool is_server(const PeerId& peer) const;

    const CallConfig config_;
    const CallHandlers handlers_;
    /// Set as the destructor begins: no handler runs, no player starts.
    std::atomic<bool> closing_{false};
    librats::Node node_;
    MediaService* media_ = nullptr;              ///< the node's
    librats::Rendezvous* rendezvous_ = nullptr;  ///< the node's, if it meets by peer id
    KnownServers known_servers_;
    /// The peers away, and what each member last said of itself: kept while
    /// it is away and as it comes back, until it says again.
    mutable std::mutex members_mutex_;
    std::set<PeerId> away_;
    std::map<PeerId, Presence> said_;

    mutable std::mutex publishing_;
    std::map<StreamId, std::unique_ptr<Published>> published_;

    PlayoutClock sync_;
    std::optional<AudioMixer> mixer_;  ///< over playback.sound, if any
    mutable std::mutex playing_;
    bool chosen_ = false;      ///< watch() was called: watched_ is whom
    Timestamp together_at_{};  ///< when keep_together() last told
    std::optional<PeerId> watched_;
    Playing<VideoPlayer> video_;  ///< the watched's, as is sound_
    Playing<AudioPlayer> sound_;
    std::map<PeerId, Member> members_;
    std::map<PeerId, float> volumes_;  ///< the voices not at 1
    float share_volume_ = 1.0f;
    Playing<DataPlayer> feed_;
    std::unique_ptr<DataPlayer> ended_feed_;  ///< writing out what it held

    /// Follows who speaks, while cameras are limited; stopped first.
    std::mutex speakers_mutex_;
    std::condition_variable_any speakers_wake_;
    std::jthread speakers_;
};

}  // namespace ratcast
