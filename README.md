# ratcast

P2P voice, video and screen sharing built on [librats](https://github.com/librats/librats) —
made for watching movies and games together: one person streams, everyone else watches.

- **libratcast** — a media transport library over librats (in the spirit of librist / SRT):
  deadline-bound delivery, FEC, congestion-driven rate control, one-to-many fan-out.
- **ratcast** — a Qt 6 desktop app for calls and streams (Windows, Linux, macOS; Android later).

Early development. See [docs/PLAN.md](docs/PLAN.md) for the design and roadmap.

## Building

```bash
git clone --recursive https://github.com/librats/ratcast.git
cd ratcast
cmake --preset debug          # or: core (libraries only, no Qt), release, asan
cmake --build --preset debug
ctest --preset debug
```

Requirements: CMake 3.21+, Ninja, a C++20 compiler, and Qt 6.5+ with Qt Quick for the app
(Debian/Ubuntu: `qt6-declarative-dev`). Without Qt Quick the app is skipped and the
libraries still build.

Already cloned without `--recursive`: `git submodule update --init --recursive`.

## C API

libratcast has a C API, [`ratcast/ratcast.h`](libratcast/include/ratcast/ratcast.h), on top of
librats' own ([`rats.h`](external/librats/src/librats/bindings/rats.h)): librats makes the node
and connects it, ratcast carries the media.

```c
rats_config_t config = {.protocol = RATCAST_PROTOCOL};  /* reaches ratcast-send */
rats_t node = rats_create(&config);
ratcast_t media;
ratcast_handlers_t handlers = {.user = app, .stream_announced = on_stream,
                               .frame_received = on_frame};
ratcast_attach(node, &handlers, NULL, &media);  /* before rats_start() */
rats_start(node);
rats_connect(node, "sender.example", 7700);

/* on_stream: subscribe to what the sender offers; a feed in order, 200 ms behind */
ratcast_subscribe(media, peer_id, stream->id, 200000);
```

A sender offers streams with `ratcast_add_stream()` and sends encoded frames with
`ratcast_send_frame()`. [`test_capi_c.c`](libratcast/tests/test_capi_c.c) is a whole program.

## License

[MIT](LICENSE)
