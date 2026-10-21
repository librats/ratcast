/* The C API from C: ratcast.h compiles as C, and a feed goes from one node to
 * another over loopback, played out in order. The viewer subscribes from its
 * stream_announced handler, as a C application would. Exits 0 on success. */

#include "ratcast/ratcast.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void sleep_ms(unsigned ms) {
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
#endif

enum { kFrames = 100, kStream = 3 };

struct viewer {
    ratcast_t media;
    atomic_int subscribed;
    atomic_int frames;
    atomic_int out_of_order;
    atomic_int corrupt;
    atomic_int withdrawn;
};

static void fill(uint8_t* bytes, size_t size, uint64_t number) {
    for (size_t i = 0; i < size; ++i) bytes[i] = (uint8_t)(number * 7 + i);
}

static void on_announced(void* user, const char* peer_id, const ratcast_stream_t* stream) {
    struct viewer* v = user;
    if (stream->kind != RATCAST_MEDIA_DATA || strcmp(stream->label, "feed") != 0) return;
    if (ratcast_subscribe(v->media, peer_id, stream->id, 30000) == RATCAST_OK) {
        atomic_store(&v->subscribed, 1);
    }
}

static void on_frame(void* user, const char* peer_id, const ratcast_frame_t* frame) {
    struct viewer* v = user;
    uint8_t expected[RATCAST_DATA_FRAME_SIZE];
    (void)peer_id;
    if (frame->number != (uint64_t)atomic_load(&v->frames)) atomic_fetch_add(&v->out_of_order, 1);
    fill(expected, sizeof expected, frame->number);
    if (frame->size != sizeof expected || memcmp(frame->data, expected, sizeof expected) != 0) {
        atomic_fetch_add(&v->corrupt, 1);
    }
    atomic_fetch_add(&v->frames, 1);
}

static void on_withdrawn(void* user, const char* peer_id, uint32_t stream) {
    struct viewer* v = user;
    (void)peer_id;
    if (stream == kStream) atomic_store(&v->withdrawn, 1);
}

static int wait_until(atomic_int* flag, int value) {
    for (int ms = 0; ms < 10000; ++ms) {
        if (atomic_load(flag) >= value) return 1;
        sleep_ms(1);
    }
    return 0;
}

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failed = 1;                                                     \
            goto done;                                                      \
        }                                                                   \
    } while (0)

int main(void) {
    int failed = 0;
    struct viewer v = {0};
    ratcast_t sender = NULL;

    rats_set_log_level(RATS_LOG_WARN);
    const rats_config_t listening = {
        .protocol = "ratcast-test/c", .bind_address = "127.0.0.1", .disable_network_monitor = 1};
    const rats_config_t dialing = {.protocol = "ratcast-test/c",
                                   .bind_address = "127.0.0.1",
                                   .dial_only = 1,
                                   .disable_network_monitor = 1};
    rats_t sender_node = rats_create(&listening);
    rats_t viewer_node = rats_create(&dialing);
    CHECK(sender_node && viewer_node);

    const ratcast_handlers_t handlers = {.user = &v,
                                         .stream_announced = on_announced,
                                         .stream_withdrawn = on_withdrawn,
                                         .frame_received = on_frame};
    CHECK(ratcast_attach(sender_node, NULL, NULL, &sender) == RATCAST_OK);
    CHECK(ratcast_attach(viewer_node, &handlers, NULL, &v.media) == RATCAST_OK);

    const ratcast_stream_t feed = {
        .id = kStream, .codec = RATCAST_CODEC_MPEG_TS, .lifetime_us = 200000, .label = "feed"};
    CHECK(ratcast_add_stream(sender, &feed, NULL) == RATCAST_OK);

    CHECK(rats_start(sender_node) == RATS_OK);
    CHECK(rats_start(viewer_node) == RATS_OK);
    CHECK(rats_connect(viewer_node, "127.0.0.1", rats_listen_port(sender_node)) == RATS_OK);
    CHECK(wait_until(&v.subscribed, 1));

    /* The subscription reaches the sender a moment after the call. */
    ratcast_outgoing_stats_t out = {0};
    for (int ms = 0; ms < 10000 && out.subscribers == 0; ++ms) {
        CHECK(ratcast_outgoing_stats(sender, kStream, &out) == RATCAST_OK);
        sleep_ms(1);
    }
    CHECK(out.subscribers == 1);

    for (uint64_t n = 0; n < kFrames; ++n) {
        uint8_t bytes[RATCAST_DATA_FRAME_SIZE];
        fill(bytes, sizeof bytes, n);
        const ratcast_frame_t frame = {.stream = kStream,
                                       .number = n,
                                       .capture_time_us = ratcast_now_us(),
                                       .keyframe = 1,
                                       .data = bytes,
                                       .size = sizeof bytes};
        CHECK(ratcast_send_frame(sender, &frame) == RATCAST_OK);
        sleep_ms(1);
    }
    ratcast_remove_stream(sender, kStream);
    CHECK(wait_until(&v.withdrawn, 1));
    CHECK(atomic_load(&v.frames) == kFrames);
    CHECK(atomic_load(&v.out_of_order) == 0);
    CHECK(atomic_load(&v.corrupt) == 0);
    printf("ratcast %s: %d frames played in order\n", ratcast_version_string(),
           atomic_load(&v.frames));

done:
    if (viewer_node) rats_destroy(viewer_node);
    if (sender_node) rats_destroy(sender_node);
    return failed;
}
