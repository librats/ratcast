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
 * What is played is the first video, camera, sound, voice and feed offered —
 * from the peer dialed, if it was dialed by peer id, from anyone otherwise —
 * each where the playback config says. A voice is a sound stream labelled
 * kVoiceLabel, a camera a video stream labelled kCameraLabel: both keep their
 * own time, while the other sound and pictures play in step (PlayoutClock);
 * the two sounds are mixed into the one output (AudioMixer). When one ends,
 * the next of its kind announced plays.
 *
 * The other side gets here by an invite (invites()): a rats:// link with this
 * node's peer id, looked for through the DHT if the call is discoverable, or
 * with one of the host's addresses and the port it listens on.
 *
 * Threads: every method may be called from any thread. Handlers run on the
 * network's, a publisher's or a player's threads, never under the call's
 * locks, and must not block or destroy the call; source_ended must not
 * unpublish the stream that ended. None runs once the destructor has begun.
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ratcast/client/audio_player.h"
#include "ratcast/client/audio_publisher.h"
#include "ratcast/client/data_player.h"
#include "ratcast/client/data_publisher.h"
#include "ratcast/client/playout_clock.h"
#include "ratcast/client/rats_url.h"
#include "ratcast/client/video_player.h"
#include "ratcast/client/video_publisher.h"
#include "ratcast/core/result.h"
#include "ratcast/engine/audio_device.h"
#include "ratcast/engine/audio_mixer.h"
#include "ratcast/engine/ts_io.h"
#include "ratcast/ratcast.h"
#include "ratcast/session/media_session.h"

#include "librats/node/node.h"

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
    /// A camera's pictures, on its player's thread, with `video`'s config.
    VideoPlayer::Sink camera = {};
    /// The device the sound and the voice play into; must outlive the call.
    AudioOutput* sound = nullptr;
    AudioPlayer::Observer heard = {};
    /// Pictures and sound of one sender in step; apart, each keeps its own
    /// time (their distance is measured all the same).
    bool sync = true;
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
    /// It takes a minute or two to converge on the open internet.
    bool discoverable = false;
    /// Who to call: an address, dialed at once, or a discoverable node's peer
    /// id, looked for through the DHT. None: wait to be called.
    std::optional<RatsUrl> dial = std::nullopt;
    /// Nodes of another protocol never connect: another version, or a test.
    std::string protocol = RATCAST_PROTOCOL;
    /// Follow the host's network changes (interfaces, addresses, sleep).
    bool watch_network = true;
    CallPlayback playback = {};
};

struct CallHandlers {
    /// A peer connected, or left: hung up, or lost.
    std::function<void(const PeerId& peer)> joined = {};
    std::function<void(const PeerId& peer)> left = {};
    /// A stream of `peer`'s started playing, or failed to (the outcome).
    std::function<void(const PeerId& peer, const StreamDescription& stream, Result<void> outcome)>
        playing = {};
    /// A stream that played ended: `peer` withdrew it, or left.
    std::function<void(const PeerId& peer, const StreamDescription& stream)> ended = {};
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
    /// rats:// links the other side can call this one by: the peer id if the
    /// call is discoverable, then each of the host's addresses (or the one it
    /// listens on) with the port. None for a call that does not listen.
    std::vector<std::string> invites() const;
    std::vector<PeerId> peers() const;
    /// The path to `peer`, sampled every 100 ms: round trip, bandwidth, loss.
    std::optional<librats::PathStats> path(const PeerId& peer) const;

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

    std::optional<ReceivedVideoStats> video_stats() const;
    std::optional<ReceivedVideoStats> camera_stats() const;
    std::optional<AudioPlayerStats> sound_stats() const;
    std::optional<AudioPlayerStats> voice_stats() const;
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
        std::unique_ptr<AudioOutput> output;  ///< a sound's input of the mixer
        std::unique_ptr<Player> player;       ///< after its output: stopped before it
        StreamDescription description;
        explicit operator bool() const noexcept { return player != nullptr; }
        /// The player first: it plays into the output.
        void reset() {
            player.reset();
            output.reset();
        }
    };

    Call(CallConfig config, CallHandlers handlers);

    SessionHandlers session_handlers();
    template <typename Source, typename Publisher, typename Config>
    Result<Publisher*> add(std::unique_ptr<Source> source, Config config);
    void announced(const PeerId& peer, const StreamDescription& description);
    void withdrawn(const PeerId& peer, StreamId stream);
    void received(const PeerId& peer, EncodedFrame frame);
    /// Start a player for `description` in `slot`, if it starts; what came
    /// of it is an event for after the lock.
    template <typename Player>
    void play(Playing<Player>& slot, const PeerId& peer, const StreamDescription& description,
              std::vector<std::function<void()>>& events);
    template <typename Player>
    static bool plays(const Playing<Player>& slot, const PeerId& peer, StreamId stream);
    std::optional<ReceivedVideoStats> stats_of(const Playing<VideoPlayer>& slot) const;
    template <typename Handler, typename... Args>
    void notify(const Handler& handler, const Args&... args) const;

    const CallConfig config_;
    const CallHandlers handlers_;
    /// Set as the destructor begins: no handler runs, no player starts.
    std::atomic<bool> closing_{false};
    librats::Node node_;
    MediaService* media_ = nullptr;  ///< the node's

    mutable std::mutex publishing_;
    std::map<StreamId, std::unique_ptr<Published>> published_;

    PlayoutClock sync_;
    std::optional<AudioMixer> mixer_;  ///< over playback.sound, if any
    mutable std::mutex playing_;
    Playing<VideoPlayer> video_;
    Playing<VideoPlayer> camera_;
    Playing<AudioPlayer> sound_;
    Playing<AudioPlayer> voice_;
    Playing<DataPlayer> feed_;
    std::unique_ptr<DataPlayer> ended_feed_;  ///< writing out what it held
};

}  // namespace ratcast
