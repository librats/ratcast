#pragma once

/**
 * @file ratcast.h
 * @brief The C API of libratcast: media streams between librats nodes.
 *
 * ratcast stands on librats' C ABI (librats/bindings/rats.h). Create a node
 * with rats_create(), enable what you want of librats on it (discovery, hole
 * punching, relays), attach ratcast with ratcast_attach() — before
 * rats_start() — and connect peers as librats does (rats_connect()). The node
 * owns what ratcast_attach() hands back: it stays valid until rats_destroy().
 *
 *     rats_config_t config = {.protocol = RATCAST_PROTOCOL};
 *     rats_t node = rats_create(&config);
 *     ratcast_t media;
 *     ratcast_handlers_t handlers = {.user = app, .frame_received = on_frame};
 *     ratcast_attach(node, &handlers, NULL, &media);
 *     ratcast_add_stream(media, &screen, NULL);  // offered to peers as they connect
 *     rats_start(node);
 *     ...
 *     ratcast_send_frame(media, &frame);         // from the encoder
 *
 * A sender offers streams, each described before its first frame
 * (ratcast_stream_t), and sends their encoded frames; every peer learns what
 * it offers (stream_announced) and subscribes to what it wants. Frames are
 * sent as librats live messages: repaired while they can still arrive within
 * the stream's lifetime, dropped whole once they cannot. ratcast never looks
 * inside a frame. Video frames reach a subscriber in decode order with every
 * frame they reference in place; a frame that cannot be decoded is dropped and
 * a keyframe asked of the sender (keyframe_needed). Sound and data frames each
 * stand alone and come as they arrive — or, subscribed with a delay, in order
 * and evenly, as SRT and librist hand on an MPEG-TS feed. A stream given
 * bitrate limits follows the paths to its subscribers, and tells the encoder
 * the bitrate to aim at (bitrate_changed).
 *
 * Path figures — round trip, bandwidth, loss — are librats': rats_path_stats().
 *
 * Errors: a fallible call returns `ratcast_error_t`, RATCAST_OK (0) on success;
 * ratcast_error_str() names a code.
 *
 * Time: microseconds. A frame's capture time is on any monotonic clock of the
 * sender's (ratcast_now_us() is ratcast's own); receivers compare capture
 * times only with each other.
 *
 * Threading: every call may be made from any thread. Handlers run on librats'
 * reactor threads, ratcast's timer thread, or — for a stream subscribed with a
 * delay — that stream's own thread; never with a lock of ratcast's held, so a
 * handler may call ratcast. Keep them short and never block in them: a
 * frame handler should only queue the frame for a decoder. Peer ids, streams
 * and frames handed to a handler are valid only for the call.
 */

#include <stddef.h>
#include <stdint.h>

#include "librats/bindings/rats.h"
#include "ratcast/export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An int-based enum in C++, as in rats.h: a binding may pass any int, and a
 * check exists to refuse the ones that are no value. */
#ifdef __cplusplus
#define RATCAST_ENUM_BASE : int
#else
#define RATCAST_ENUM_BASE
#endif

typedef struct ratcast_media* ratcast_t;

/* The handshake protocol (rats_config_t::protocol) of ratcast's own tools and
 * app: a node made with it reaches them, and only nodes of the same protocol
 * connect at all. */
#define RATCAST_PROTOCOL "ratcast/0"

/* ─── Results ─────────────────────────────────────────────────────────────── */

typedef enum RATCAST_ENUM_BASE {
    RATCAST_OK = 0,
    RATCAST_ERR_INVALID_ARG = 1,     /* NULL, out of range or malformed: a peer id, a stream
                                      * description, a frame number that does not increase,
                                      * a frame too large */
    RATCAST_ERR_ALREADY_STARTED = 2, /* ratcast_attach() after rats_start() */
    RATCAST_ERR_EXISTS = 3,          /* ratcast is attached to the node already, or a stream
                                      * of this id is offered already */
    RATCAST_ERR_NOT_FOUND = 4,       /* no such stream: not offered by this node, or not
                                      * announced by that peer */
    RATCAST_ERR_INTERNAL = 5
} ratcast_error_t;

/** Static name of an error code, e.g. "RATCAST_ERR_NOT_FOUND" (never NULL). */
RATCAST_EXPORT const char* ratcast_error_str(ratcast_error_t err);

/* ─── Streams ─────────────────────────────────────────────────────────────── */

typedef enum RATCAST_ENUM_BASE {
    RATCAST_MEDIA_VIDEO = 1,
    RATCAST_MEDIA_AUDIO = 2,
    RATCAST_MEDIA_DATA = 3 /* bytes ratcast does not look into: an MPEG-TS feed */
} ratcast_media_kind_t;

/* The bitstream format of a stream's frames, 1..255. ratcast never looks
 * inside a frame, so a value not named here is carried through unchanged
 * (give its kind, as no build of ratcast knows it). */
typedef enum RATCAST_ENUM_BASE {
    RATCAST_CODEC_H264 = 1,
    RATCAST_CODEC_H265 = 2,
    RATCAST_CODEC_AV1 = 3,
    RATCAST_CODEC_OPUS = 64,
    RATCAST_CODEC_MPEG_TS = 128 /* whole 188-byte packets, a few to a frame */
} ratcast_codec_t;

#define RATCAST_MAX_LABEL 64u
#define RATCAST_MAX_CODEC_CONFIG (64u * 1024u)
#define RATCAST_MAX_LIFETIME_US 30000000u
#define RATCAST_MAX_DIMENSION 16384u

/* What a sender tells its peers about a stream before any frame of it, and
 * again whenever it changes. Zero in a field is its default where one is
 * named. */
typedef struct {
    uint32_t id; /* this node's own number for the stream */
    ratcast_codec_t codec;
    ratcast_media_kind_t kind; /* 0 → the codec's; needed for a codec not named above */
    struct {
        uint32_t width, height; /* 1..RATCAST_MAX_DIMENSION */
        /* Nominal frames per second as a fraction (30000/1001); a numerator
         * of 0 when the source has no fixed rate, as a screen that sends only
         * what changes. A denominator of 0 is 1. */
        uint32_t frame_rate_num, frame_rate_den;
    } video; /* a video stream's */
    struct {
        uint32_t sample_rate; /* Hz, not 0 */
        uint32_t channels;    /* 1..255 */
    } audio;                  /* a sound stream's */
    /* How long after it is sent a frame is still worth delivering: what repair
     * has to work in, SRT's latency, librist's buffer. Past it a frame is
     * dropped, not late. 1..RATCAST_MAX_LIFETIME_US. */
    uint32_t lifetime_us;
    const char* label; /* what it is to the application ("screen", "mic");
                        * at most RATCAST_MAX_LABEL bytes. NULL → "" */
    /* Out-of-band decoder configuration (avcC, hvcC, av1C, OpusHead); may be
     * empty. At most RATCAST_MAX_CODEC_CONFIG bytes. */
    const void* codec_config;
    size_t codec_config_size;
} ratcast_stream_t;

/* The bitrates rate control keeps a stream between, bits per second:
 * 0 < min_bps <= start_bps <= max_bps. */
typedef struct {
    uint32_t min_bps;
    uint32_t start_bps;
    uint32_t max_bps;
} ratcast_bitrate_t;

/* ─── Frames ──────────────────────────────────────────────────────────────── */

#define RATCAST_MAX_REFERENCES 8u
#define RATCAST_MAX_TEMPORAL_LAYER 7u
#define RATCAST_MAX_SPATIAL_LAYER 3u
/* Six MPEG-TS packets: the most of a data stream's frame that one librats
 * datagram carries, so that one lost costs those packets and no more. */
#define RATCAST_DATA_FRAME_SIZE 1128u

typedef struct {
    uint32_t stream;
    uint64_t number;         /* position in decode order, from 0; increasing */
    int64_t capture_time_us; /* when its content was captured, on the sender's clock;
                              * not negative */
    int keyframe;            /* non-zero: a decoder can start here (every sound and
                              * data frame is one) */
    uint8_t temporal_layer;  /* 0 (base) .. RATCAST_MAX_TEMPORAL_LAYER */
    uint8_t spatial_layer;   /* 0 (base) .. RATCAST_MAX_SPATIAL_LAYER */
    /* Earlier frames of the stream this one cannot be decoded without, each
     * below `number`: what lets a receiver drop a frame whose reference was
     * lost rather than feed a decoder garbage. */
    uint8_t reference_count; /* 0 .. RATCAST_MAX_REFERENCES */
    uint64_t references[RATCAST_MAX_REFERENCES];
    const void* data; /* the encoded frame */
    size_t size;
} ratcast_frame_t;

/* ─── Attaching ───────────────────────────────────────────────────────────── */

/* What ratcast tells the application. Any of them may be NULL. */
typedef struct {
    void* user; /* handed to each */
    /** A peer offers a stream, or changed the description of one it offers. */
    void (*stream_announced)(void* user, const char* peer_id, const ratcast_stream_t* stream);
    /** A peer no longer offers a stream: it withdrew it or disconnected. For a
     *  stream subscribed with a delay, once the frames held have been handed on. */
    void (*stream_withdrawn)(void* user, const char* peer_id, uint32_t stream);
    /** A frame of a subscribed stream (see ratcast_subscribe()). */
    void (*frame_received)(void* user, const char* peer_id, const ratcast_frame_t* frame);
    /** The encoder of a stream this node sends should make its next frame a
     *  keyframe: a subscriber joined, or lost what it needed. */
    void (*keyframe_needed)(void* user, uint32_t stream);
    /** The encoder of a rate-controlled stream should aim at a new bitrate. */
    void (*bitrate_changed)(void* user, uint32_t stream, uint32_t bits_per_second);
} ratcast_handlers_t;

/* Zero in any field is the default named beside it. */
typedef struct {
    /* Least time between two keyframe requests a receiver sends for a stream
     * it cannot decode. Keep it above min_keyframe_interval_us. 0 → 250 ms */
    uint32_t keyframe_retry_us;
    /* Least time between two keyframes asked of an encoder. 0 → 200 ms */
    uint32_t min_keyframe_interval_us;
    /* Frames a receiver holds per stream while a gap waits to fill. 0 → 128 */
    uint32_t max_pending_frames;
} ratcast_config_t;

/** Attach ratcast to `node`, before rats_start(). `handlers` and `config` may
 *  be NULL; both are copied. The handle is written to *out and is the node's:
 *  valid until rats_destroy(). RATCAST_ERR_ALREADY_STARTED after rats_start(),
 *  RATCAST_ERR_EXISTS if ratcast is attached to the node already. */
RATCAST_EXPORT ratcast_error_t ratcast_attach(rats_t node, const ratcast_handlers_t* handlers,
                                              const ratcast_config_t* config, ratcast_t* out);

/* ─── Streams this node sends ─────────────────────────────────────────────── */

/** Offer a stream to every peer, now and as they connect; with `bitrate`
 *  (may be NULL), rate-controlled from its start on. RATCAST_ERR_EXISTS for an
 *  id offered already. May be called before rats_start(). */
RATCAST_EXPORT ratcast_error_t ratcast_add_stream(ratcast_t media, const ratcast_stream_t* stream,
                                                  const ratcast_bitrate_t* bitrate);

/** Replace an offered stream's description (its id names it) and announce it
 *  again. */
RATCAST_EXPORT ratcast_error_t ratcast_update_stream(ratcast_t media,
                                                     const ratcast_stream_t* stream);

/** Stop offering a stream; its subscribers are told it is gone. */
RATCAST_EXPORT void ratcast_remove_stream(ratcast_t media, uint32_t stream);

/** Send a frame of an offered stream to its subscribers. The bytes are copied:
 *  `frame->data` may be reused once this returns. Frame numbers must
 *  increase. */
RATCAST_EXPORT ratcast_error_t ratcast_send_frame(ratcast_t media, const ratcast_frame_t* frame);

/* ─── Streams peers send ──────────────────────────────────────────────────── */

/** Receive `peer_id`'s `stream`, which it has announced (stream_announced;
 *  RATCAST_ERR_NOT_FOUND otherwise), at frame_received.
 *
 *  `delay_us` 0: each frame as it comes, on a reactor thread — video from its
 *  next keyframe on, in decode order; sound and data as they arrive, for a
 *  jitter buffer to order. Non-zero: in order and evenly, each `delay_us`
 *  past the fastest frames, on a thread of the stream's own; what did not
 *  come in time is skipped. That is how a data stream — an MPEG-TS feed —
 *  plays out, what SRT and librist do; repair needs the time, so up to the
 *  stream's lifetime less the path's one-way delay is what helps.
 *
 *  Subscribing again with another delay changes it, starting afresh. */
RATCAST_EXPORT ratcast_error_t ratcast_subscribe(ratcast_t media, const char* peer_id,
                                                 uint32_t stream, uint32_t delay_us);

/** Stop receiving a stream. Once this returns, a stream subscribed with a
 *  delay hands nothing more to frame_received; until then its thread may.
 *  May be called from a handler, that stream's own included. */
RATCAST_EXPORT ratcast_error_t ratcast_unsubscribe(ratcast_t media, const char* peer_id,
                                                   uint32_t stream);

/** Ask for a keyframe of a subscribed video stream, after a decoder error.
 *  Until it comes, frames that depend on earlier ones are dropped. */
RATCAST_EXPORT ratcast_error_t ratcast_request_keyframe(ratcast_t media, const char* peer_id,
                                                        uint32_t stream);

/* ─── Statistics ──────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t subscribers;
    uint64_t frames;             /* frames given to ratcast_send_frame() */
    uint64_t keyframes;          /* of which keyframes */
    uint64_t messages;           /* frame messages sent: one per frame per subscriber */
    uint64_t bytes;              /* bytes of those messages */
    uint64_t awaiting_keyframe;  /* not sent: the subscriber waits for a keyframe */
    uint64_t backpressure_drops; /* not sent: the subscriber's queue was full */
    uint64_t keyframe_requests;  /* keyframe requests received from subscribers */
    uint64_t keyframes_asked;    /* times keyframe_needed was called */
    uint32_t bitrate_bps;        /* what rate control aims at now; 0 without it */
    uint64_t bitrate_cuts;       /* times rate control lowered the bitrate */
    uint64_t bitrate_raises;     /* and raised it */
} ratcast_outgoing_stats_t;

typedef struct {
    int subscribed;
    uint64_t bytes; /* frame message bytes received while subscribed */
    uint64_t frames_received;
    uint64_t frames_delivered;   /* handed on: to frame_received, or to the delay */
    uint64_t frames_late;        /* arrived after their turn, or twice */
    uint64_t frames_superseded;  /* video: waiting when a later keyframe made them pointless */
    uint64_t frames_undecodable; /* video: dropped for a lost reference */
    uint64_t frames_lost;        /* video: never arrived, given up on */
    uint64_t keyframe_requests;  /* sent for this stream */
    /* Subscribed with a delay: what playing out did. */
    uint64_t played;    /* handed to frame_received */
    uint64_t play_lost; /* skipped: not there in time */
    uint64_t play_late; /* came after their time, or twice: dropped */
} ratcast_incoming_stats_t;

/** What an offered stream sent. RATCAST_ERR_NOT_FOUND if it is not offered. */
RATCAST_EXPORT ratcast_error_t ratcast_outgoing_stats(ratcast_t media, uint32_t stream,
                                                      ratcast_outgoing_stats_t* out);

/** What came of a peer's stream. RATCAST_ERR_NOT_FOUND if the peer does not
 *  offer it. */
RATCAST_EXPORT ratcast_error_t ratcast_incoming_stats(ratcast_t media, const char* peer_id,
                                                      uint32_t stream,
                                                      ratcast_incoming_stats_t* out);

/** Messages from peers refused as malformed or of an unknown kind. */
RATCAST_EXPORT uint64_t ratcast_rejected_messages(ratcast_t media);

/* ─── Process-wide ────────────────────────────────────────────────────────── */

/** ratcast's clock, for capture times: microseconds, monotonic. */
RATCAST_EXPORT int64_t ratcast_now_us(void);

/** Library version, e.g. "0.1.0". */
RATCAST_EXPORT const char* ratcast_version_string(void);
/** (major << 16) | (minor << 8) | patch. */
RATCAST_EXPORT uint32_t ratcast_abi(void);

#undef RATCAST_ENUM_BASE

#ifdef __cplusplus
}
#endif
