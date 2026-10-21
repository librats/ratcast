// The C API (ratcast/ratcast.h): a MediaSession on a C caller's librats node,
// with the conversions between its C structs and libratcast's own.

#include "ratcast/ratcast.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ratcast/session/media_service.h"
#include "ratcast/stream/delivery_thread.h"
#include "ratcast/version.h"

#include "librats/bindings/rats_extend.h"

using namespace ratcast;

namespace {

/// What the node knows ratcast's subsystem by: one per node.
constexpr const char* kSubsystemName = "ratcast";

/// Nothing may unwind across the C API: a call that throws (no memory) fails.
template <typename Call>
ratcast_error_t guarded(Call&& call) noexcept {
    try {
        return call();
    } catch (...) {
        return RATCAST_ERR_INTERNAL;
    }
}

std::optional<PeerId> parse_peer(const char* hex) {
    if (!hex) return std::nullopt;
    return PeerId::from_hex(hex);
}

std::optional<StreamDescription> from_c(const ratcast_stream_t& c) {
    if (c.codec < 1 || c.codec > 255) return std::nullopt;
    if (c.codec_config_size > StreamDescription::kMaxCodecConfigSize) return std::nullopt;
    if (!c.codec_config && c.codec_config_size > 0) return std::nullopt;
    const size_t label_size = c.label ? strnlen(c.label, StreamDescription::kMaxLabelSize + 1) : 0;
    if (label_size > StreamDescription::kMaxLabelSize) return std::nullopt;

    if (c.kind != 0 && (c.kind < RATCAST_MEDIA_VIDEO || c.kind > RATCAST_MEDIA_DATA)) {
        return std::nullopt;
    }

    StreamDescription d;
    d.id = c.id;
    d.codec = static_cast<Codec>(c.codec);
    const std::optional<MediaKind> kind =
        c.kind != 0 ? std::optional(static_cast<MediaKind>(c.kind)) : media_kind_of(d.codec);
    if (!kind) return std::nullopt;
    switch (*kind) {
        case MediaKind::Video:
            d.format = VideoFormat{c.video.width, c.video.height, c.video.frame_rate_num,
                                   c.video.frame_rate_den == 0 ? 1 : c.video.frame_rate_den};
            break;
        case MediaKind::Audio:
            if (c.audio.channels > 255) return std::nullopt;
            d.format = AudioFormat{c.audio.sample_rate, static_cast<uint8_t>(c.audio.channels)};
            break;
        case MediaKind::Data: d.format = DataFormat{}; break;
    }
    d.lifetime = Duration(c.lifetime_us);
    d.label.assign(c.label ? c.label : "", label_size);
    if (c.codec_config_size > 0) {
        d.codec_config = Buffer::copy_of(
            std::span(static_cast<const uint8_t*>(c.codec_config), c.codec_config_size));
    }
    if (!is_valid(d)) return std::nullopt;
    return d;
}

/// Points into `d`: valid while it is.
ratcast_stream_t to_c(const StreamDescription& d) {
    ratcast_stream_t c{};
    c.id = d.id;
    c.codec = static_cast<ratcast_codec_t>(d.codec);
    c.kind = static_cast<ratcast_media_kind_t>(d.kind());
    if (const VideoFormat* video = d.video()) {
        c.video = {video->width, video->height, video->frame_rate_num, video->frame_rate_den};
    }
    if (const AudioFormat* audio = d.audio()) c.audio = {audio->sample_rate, audio->channels};
    c.lifetime_us = static_cast<uint32_t>(d.lifetime.count());
    c.label = d.label.c_str();
    c.codec_config = d.codec_config.empty() ? nullptr : d.codec_config.data();
    c.codec_config_size = d.codec_config.size();
    return c;
}

std::optional<FrameInfo> from_c(const ratcast_frame_t& c) {
    if (c.reference_count > FrameReferences::kCapacity) return std::nullopt;
    FrameInfo info;
    info.stream = c.stream;
    info.number = c.number;
    info.capture_time = Timestamp(Duration(c.capture_time_us));
    info.keyframe = c.keyframe != 0;
    info.temporal_layer = c.temporal_layer;
    info.spatial_layer = c.spatial_layer;
    for (size_t i = 0; i < c.reference_count; ++i) info.references.push_back(c.references[i]);
    return info;
}

/// Points into `frame`: valid while it is.
ratcast_frame_t to_c(const EncodedFrame& frame) {
    const FrameInfo& info = frame.info;
    ratcast_frame_t c{};
    c.stream = info.stream;
    c.number = info.number;
    c.capture_time_us = info.capture_time.time_since_epoch().count();
    c.keyframe = info.keyframe ? 1 : 0;
    c.temporal_layer = info.temporal_layer;
    c.spatial_layer = info.spatial_layer;
    for (FrameNumber reference : info.references) c.references[c.reference_count++] = reference;
    c.data = frame.payload.data();
    c.size = frame.payload.size();
    return c;
}

SessionConfig session_config(const ratcast_config_t* c) {
    SessionConfig config;
    if (!c) return config;
    if (c->keyframe_retry_us != 0) config.keyframe_retry = Duration(c->keyframe_retry_us);
    if (c->min_keyframe_interval_us != 0) {
        config.min_keyframe_interval = Duration(c->min_keyframe_interval_us);
    }
    if (c->max_pending_frames != 0) config.max_pending_frames = c->max_pending_frames;
    return config;
}

struct StreamKey {
    PeerId peer;
    StreamId stream = 0;
    friend bool operator==(const StreamKey&, const StreamKey&) = default;
};

struct StreamKeyHash {
    size_t operator()(const StreamKey& key) const noexcept {
        return PeerId::Hash{}(key.peer) * 31 + key.stream;
    }
};

/// A peer's stream subscribed with a delay: frames in order and evenly, on a
/// thread of its own.
struct Playout {
    Playout(const ratcast_handlers_t& handlers, const PeerId& peer, StreamId stream, Duration delay)
        : handlers(handlers),
          peer(peer),
          peer_id(peer.to_hex()),
          stream(stream),
          delivery(
              {.latency = delay}, [this](std::span<const EncodedFrame> frames) { play(frames); },
              [this] { drained(); }) {}

    bool is(const PeerId& other_peer, StreamId other_stream) const noexcept {
        return stream == other_stream && peer == other_peer;
    }

    void play(std::span<const EncodedFrame> frames) const {
        if (!handlers.frame_received) return;
        for (const EncodedFrame& frame : frames) {
            // A handler may unsubscribe amid frames played together.
            if (stopped.load(std::memory_order_acquire)) return;
            const ratcast_frame_t c = to_c(frame);
            handlers.frame_received(handlers.user, peer_id.c_str(), &c);
        }
    }

    /// Withdrawn, and all it held handed on (or dropped, unsubscribed): the
    /// application hears of it now.
    void drained() const {
        if (handlers.stream_withdrawn) {
            handlers.stream_withdrawn(handlers.user, peer_id.c_str(), stream);
        }
    }

    const ratcast_handlers_t handlers;
    const PeerId peer;
    const std::string peer_id;  ///< as the handlers take it
    const StreamId stream;
    std::atomic<bool> stopped{false};  ///< by release(): nothing more is played
    DeliveryThread delivery;           ///< last: stops before what its thread uses goes
};

}  // namespace

/// The handle behind ratcast_t: a subsystem the node owns, the session on it,
/// and the streams subscribed with a delay.
struct ratcast_media final : public librats::Subsystem {
    ratcast_media(const ratcast_handlers_t& handlers, const SessionConfig& config)
        : handlers_(handlers), service_(session_handlers(), config) {}

    void attach(librats::NodeContext& ctx) override { service_.attach(ctx); }
    void start() override { service_.start(); }
    void stop() override { service_.stop(); }

    MediaSession& session() noexcept { return service_.session(); }

    /// Hand `peer`'s `stream` on with `delay`, or as it comes with none; the
    /// playout it replaces is stopped.
    void play(const PeerId& peer, StreamId stream, Duration delay) {
        std::shared_ptr<Playout> playout;
        if (delay > Duration::zero()) {
            playout = std::make_shared<Playout>(handlers_, peer, stream, delay);
            playout->delivery.start();
        }
        std::vector<std::shared_ptr<Playout>> taken;
        {
            std::lock_guard lock(mutex_);
            taken = take(peer, stream);
            if (playout) playouts_[{peer, stream}] = std::move(playout);
        }
        release(std::move(taken));
    }

    /// Stop handing `peer`'s `stream` on, held frames and all.
    void stop_playing(const PeerId& peer, StreamId stream) {
        std::vector<std::shared_ptr<Playout>> taken;
        {
            std::lock_guard lock(mutex_);
            taken = take(peer, stream);
        }
        release(std::move(taken));
    }

    /// What playing `peer`'s `stream` out did, if it is played.
    std::optional<DeliveryBuffer::Stats> play_stats(const PeerId& peer, StreamId stream) {
        std::lock_guard lock(mutex_);
        if (const auto it = playouts_.find({peer, stream}); it != playouts_.end()) {
            return it->second->delivery.stats();
        }
        for (const std::shared_ptr<Playout>& playout : ending_) {
            if (playout->is(peer, stream)) return playout->delivery.stats();
        }
        return std::nullopt;
    }

private:
    SessionHandlers session_handlers() {
        return {
            .stream_announced =
                [this](const PeerId& peer, const StreamDescription& description) {
                    if (!handlers_.stream_announced) return;
                    const ratcast_stream_t c = to_c(description);
                    handlers_.stream_announced(handlers_.user, peer.to_hex().c_str(), &c);
                },
            .stream_withdrawn = [this](const PeerId& peer,
                                       StreamId stream) { withdrawn(peer, stream); },
            .frame_received = [this](const PeerId& peer,
                                     EncodedFrame frame) { received(peer, std::move(frame)); },
            .keyframe_needed =
                [this](StreamId stream) {
                    if (handlers_.keyframe_needed)
                        handlers_.keyframe_needed(handlers_.user, stream);
                },
            .bitrate_changed =
                [this](StreamId stream, uint32_t bits_per_second) {
                    if (handlers_.bitrate_changed) {
                        handlers_.bitrate_changed(handlers_.user, stream, bits_per_second);
                    }
                },
        };
    }

    void received(const PeerId& peer, EncodedFrame frame) {
        if (std::shared_ptr<Playout> playout = find(peer, frame.info.stream)) {
            playout->delivery.push(std::move(frame));
            return;
        }
        if (!handlers_.frame_received) return;
        const ratcast_frame_t c = to_c(frame);
        handlers_.frame_received(handlers_.user, peer.to_hex().c_str(), &c);
    }

    /// A played stream hands on what it holds first, and tells the application
    /// then (Playout::drained).
    void withdrawn(const PeerId& peer, StreamId stream) {
        std::vector<std::shared_ptr<Playout>> ended;
        bool played = false;
        {
            std::lock_guard lock(mutex_);
            ended = reap();
            if (const auto it = playouts_.find({peer, stream}); it != playouts_.end()) {
                it->second->delivery.finish();
                ending_.push_back(std::move(it->second));
                playouts_.erase(it);
                played = true;
            }
        }
        release(std::move(ended));
        if (!played && handlers_.stream_withdrawn) {
            handlers_.stream_withdrawn(handlers_.user, peer.to_hex().c_str(), stream);
        }
    }

    std::shared_ptr<Playout> find(const PeerId& peer, StreamId stream) {
        std::lock_guard lock(mutex_);
        const auto it = playouts_.find({peer, stream});
        return it == playouts_.end() ? nullptr : it->second;
    }

    /// Under the lock: the playouts of `peer`'s `stream`, playing or ending,
    /// and those that have ended, out of the lists for release().
    std::vector<std::shared_ptr<Playout>> take(const PeerId& peer, StreamId stream) {
        std::vector<std::shared_ptr<Playout>> taken = reap();
        if (const auto it = playouts_.find({peer, stream}); it != playouts_.end()) {
            taken.push_back(std::move(it->second));
            playouts_.erase(it);
        }
        std::erase_if(ending_, [&](std::shared_ptr<Playout>& playout) {
            if (!playout->is(peer, stream)) return false;
            taken.push_back(std::move(playout));
            return true;
        });
        return taken;
    }

    /// Under the lock: the playouts whose threads have ended, for release().
    std::vector<std::shared_ptr<Playout>> reap() {
        std::vector<std::shared_ptr<Playout>> ended;
        std::erase_if(ending_, [&](std::shared_ptr<Playout>& playout) {
            if (!playout->delivery.ended()) return false;
            ended.push_back(std::move(playout));
            return true;
        });
        return ended;
    }

    /// Out of the lock: stop the playouts taken and let them go, each joining
    /// its thread. One stopped on its own thread — a handler of its stream
    /// unsubscribed — ends once that handler returns: it is kept until then,
    /// for a later call to reap.
    void release(std::vector<std::shared_ptr<Playout>> taken) {
        for (std::shared_ptr<Playout>& playout : taken) {
            playout->stopped.store(true, std::memory_order_release);
            playout->delivery.stop();
            if (playout->delivery.ended()) continue;
            std::lock_guard lock(mutex_);
            ending_.push_back(std::move(playout));
        }
    }

    const ratcast_handlers_t handlers_;

    std::mutex mutex_;  ///< the playouts, between the network, the application and the timer
    std::unordered_map<StreamKey, std::shared_ptr<Playout>, StreamKeyHash> playouts_;
    /// Withdrawn and handing on what they hold, or stopped by a handler of
    /// their own: their threads end by themselves.
    std::vector<std::shared_ptr<Playout>> ending_;

    MediaService service_;  ///< last, so first to go: its handlers use the rest
};

// Every function below was declared extern "C" by ratcast.h.

const char* ratcast_error_str(ratcast_error_t err) {
    switch (err) {
        case RATCAST_OK: return "RATCAST_OK";
        case RATCAST_ERR_INVALID_ARG: return "RATCAST_ERR_INVALID_ARG";
        case RATCAST_ERR_ALREADY_STARTED: return "RATCAST_ERR_ALREADY_STARTED";
        case RATCAST_ERR_EXISTS: return "RATCAST_ERR_EXISTS";
        case RATCAST_ERR_NOT_FOUND: return "RATCAST_ERR_NOT_FOUND";
        case RATCAST_ERR_INTERNAL: return "RATCAST_ERR_INTERNAL";
    }
    return "RATCAST_ERR_UNKNOWN";
}

ratcast_error_t ratcast_attach(rats_t node, const ratcast_handlers_t* handlers,
                               const ratcast_config_t* config, ratcast_t* out) {
    if (!node || !out) return RATCAST_ERR_INVALID_ARG;
    return guarded([&] {
        auto media = std::make_unique<ratcast_media>(handlers ? *handlers : ratcast_handlers_t{},
                                                     session_config(config));
        ratcast_media* handle = media.get();
        switch (rats_attach_subsystem(node, kSubsystemName, std::move(media))) {
            case RATS_OK: *out = handle; return RATCAST_OK;
            case RATS_ERR_ALREADY_STARTED: return RATCAST_ERR_ALREADY_STARTED;
            case RATS_ERR_DECLINED: return RATCAST_ERR_EXISTS;
            default: return RATCAST_ERR_INTERNAL;
        }
    });
}

ratcast_error_t ratcast_add_stream(ratcast_t media, const ratcast_stream_t* stream,
                                   const ratcast_bitrate_t* bitrate) {
    if (!media || !stream) return RATCAST_ERR_INVALID_ARG;
    return guarded([&] {
        const std::optional<StreamDescription> description = from_c(*stream);
        if (!description) return RATCAST_ERR_INVALID_ARG;
        std::optional<BitrateLimits> limits;
        if (bitrate) {
            limits = BitrateLimits{bitrate->min_bps, bitrate->start_bps, bitrate->max_bps};
            if (!is_valid(*limits)) return RATCAST_ERR_INVALID_ARG;
        }
        if (media->session().add_stream(*description, limits)) return RATCAST_OK;
        // Valid as it is: the id must be in use.
        return media->session().outgoing_stats(description->id) ? RATCAST_ERR_EXISTS
                                                                : RATCAST_ERR_INTERNAL;
    });
}

ratcast_error_t ratcast_update_stream(ratcast_t media, const ratcast_stream_t* stream) {
    if (!media || !stream) return RATCAST_ERR_INVALID_ARG;
    return guarded([&] {
        const std::optional<StreamDescription> description = from_c(*stream);
        if (!description) return RATCAST_ERR_INVALID_ARG;
        if (media->session().update_stream(*description)) return RATCAST_OK;
        return media->session().outgoing_stats(description->id) ? RATCAST_ERR_INVALID_ARG
                                                                : RATCAST_ERR_NOT_FOUND;
    });
}

void ratcast_remove_stream(ratcast_t media, uint32_t stream) {
    if (media) media->session().remove_stream(stream);
}

ratcast_error_t ratcast_send_frame(ratcast_t media, const ratcast_frame_t* frame) {
    if (!media || !frame || (!frame->data && frame->size > 0)) return RATCAST_ERR_INVALID_ARG;
    // Refused before a byte is copied: no message is larger.
    if (frame->size > RATS_LIVE_MAX_PAYLOAD) return RATCAST_ERR_INVALID_ARG;
    return guarded([&] {
        const std::optional<FrameInfo> info = from_c(*frame);
        if (!info) return RATCAST_ERR_INVALID_ARG;
        MutableBuffer payload = allocate_frame_payload(frame->size);
        payload.append(std::span(static_cast<const uint8_t*>(frame->data), frame->size));
        if (media->session().send_frame(frame->stream, *info, std::move(payload)))
            return RATCAST_OK;
        return media->session().outgoing_stats(frame->stream) ? RATCAST_ERR_INVALID_ARG
                                                              : RATCAST_ERR_NOT_FOUND;
    });
}

ratcast_error_t ratcast_subscribe(ratcast_t media, const char* peer_id, uint32_t stream,
                                  uint32_t delay_us) {
    const std::optional<PeerId> peer = parse_peer(peer_id);
    if (!media || !peer) return RATCAST_ERR_INVALID_ARG;
    return guarded([&] {
        if (!media->session().incoming_stats(*peer, stream)) return RATCAST_ERR_NOT_FOUND;
        // Played before subscribed, so that the first frame finds its playout.
        media->play(*peer, stream, Duration(delay_us));
        if (media->session().subscribe(*peer, stream)) return RATCAST_OK;
        media->stop_playing(*peer, stream);
        return RATCAST_ERR_NOT_FOUND;  // withdrawn meanwhile
    });
}

ratcast_error_t ratcast_unsubscribe(ratcast_t media, const char* peer_id, uint32_t stream) {
    const std::optional<PeerId> peer = parse_peer(peer_id);
    if (!media || !peer) return RATCAST_ERR_INVALID_ARG;
    return guarded([&] {
        media->session().unsubscribe(*peer, stream);
        media->stop_playing(*peer, stream);
        return RATCAST_OK;
    });
}

ratcast_error_t ratcast_request_keyframe(ratcast_t media, const char* peer_id, uint32_t stream) {
    const std::optional<PeerId> peer = parse_peer(peer_id);
    if (!media || !peer) return RATCAST_ERR_INVALID_ARG;
    return guarded([&] {
        if (!media->session().incoming_stats(*peer, stream)) return RATCAST_ERR_NOT_FOUND;
        media->session().request_keyframe(*peer, stream);
        return RATCAST_OK;
    });
}

ratcast_error_t ratcast_outgoing_stats(ratcast_t media, uint32_t stream,
                                       ratcast_outgoing_stats_t* out) {
    if (!media || !out) return RATCAST_ERR_INVALID_ARG;
    return guarded([&] {
        const std::optional<OutgoingStreamStats> s = media->session().outgoing_stats(stream);
        if (!s) return RATCAST_ERR_NOT_FOUND;
        *out = {
            .subscribers = static_cast<uint32_t>(s->subscribers),
            .frames = s->frames,
            .keyframes = s->keyframes,
            .messages = s->messages,
            .bytes = s->bytes,
            .awaiting_keyframe = s->awaiting_keyframe,
            .backpressure_drops = s->backpressure_drops,
            .keyframe_requests = s->keyframe_requests,
            .keyframes_asked = s->keyframes_asked,
            .bitrate_bps = s->bitrate,
            .bitrate_cuts = s->bitrate_cuts,
            .bitrate_raises = s->bitrate_raises,
        };
        return RATCAST_OK;
    });
}

ratcast_error_t ratcast_incoming_stats(ratcast_t media, const char* peer_id, uint32_t stream,
                                       ratcast_incoming_stats_t* out) {
    const std::optional<PeerId> peer = parse_peer(peer_id);
    if (!media || !peer || !out) return RATCAST_ERR_INVALID_ARG;
    return guarded([&] {
        const std::optional<IncomingStreamStats> s = media->session().incoming_stats(*peer, stream);
        if (!s) return RATCAST_ERR_NOT_FOUND;
        const DeliveryBuffer::Stats played =
            media->play_stats(*peer, stream).value_or(DeliveryBuffer::Stats{});
        *out = {
            .subscribed = s->subscribed ? 1 : 0,
            .bytes = s->bytes,
            .frames_received = s->frames.received,
            .frames_delivered = s->frames.delivered,
            .frames_late = s->frames.late,
            .frames_superseded = s->frames.superseded,
            .frames_undecodable = s->frames.undecodable,
            .frames_lost = s->frames.lost,
            .keyframe_requests = s->frames.keyframe_requests,
            .played = played.delivered,
            .play_lost = played.lost,
            .play_late = played.late,
        };
        return RATCAST_OK;
    });
}

uint64_t ratcast_rejected_messages(ratcast_t media) {
    return media ? media->session().rejected_messages() : 0;
}

int64_t ratcast_now_us(void) { return SteadyClock::instance().now().time_since_epoch().count(); }

const char* ratcast_version_string(void) { return version(); }

uint32_t ratcast_abi(void) {
    return (uint32_t{RATCAST_VERSION_MAJOR} << 16) | (uint32_t{RATCAST_VERSION_MINOR} << 8) |
           uint32_t{RATCAST_VERSION_PATCH};
}
