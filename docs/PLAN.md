# ratcast — plan

ratcast is two things built together:

- **libratcast**: a media transport library on top of [librats](https://github.com/librats/librats),
  meant to stand on its own against librist, SRT and the WebRTC media stack.
- **ratcast**: a Discord-calls-like desktop application (later Android) for voice, video
  and screen sharing, built for the case where **one person streams and the others watch**:
  movies and games together.

## Decisions

| Topic | Decision |
|---|---|
| Transport | librats live messages (deadline-bound, FEC, BBRv3, `PathStats`), extended in-tree as needed |
| UI | Qt 6 / QML (QRhi for zero-copy video), same code for Android later |
| Servers | Optional SFU / relay nodes are allowed; P2P stays the default for small rooms |
| Scope, first stage | Calls and streams. Text chat is designed for (room protocol has a place for it), built later |
| License | MIT, open source. No GPL dependencies (no x264) |
| Language | C++20 for ratcast, librats stays C++17 |

## Layers

```
app/          Qt UI: rooms, call view, settings, tray
client/       UI-agnostic core: identity, rooms, members, chat, call orchestration
engine/       media I/O: capture, encode/decode, audio processing, render helpers
libratcast/   media transport: frames, layers, jitter buffer, sync, rate control,
              fan-out tree, SFrame E2EE, C API
librats       secure P2P transport (external/librats)
```

Each layer uses only those below it. `libratcast` knows nothing of codecs or capture: it moves
encoded frames, so it is useful without the rest and the SFU links only it.

## libratcast

- **Media framing** on top of live messages: stream id, sequence, capture timestamp, frame type,
  a dependency descriptor (which frames this one references, which spatial/temporal layer it is
  in), audio level for speaker selection without decoding.
- **Latency profiles**, the one knob trading latency for quality (librist's `buffer`):
  `call` (~150 ms), `game` (~100–200 ms), `cinema` (0.8–2 s: retransmission has time to repair
  nearly everything, so the picture stays clean on long, lossy paths).
- **Rate control** fed by `PathStats` (`bandwidth_bps`, `live_expired`, repair overhead):
  fast down, careful up.
- **Loss recovery**: keyframe requests with debounce and aggregation, intra-refresh for `game`,
  reference invalidation driven by per-message delivery feedback.
- **One-to-many**: a capacity-aware forwarding tree (viewers with spare uplink re-forward without
  transcoding), simulcast / SVC layer selection per receiver, SFU mode for big rooms and hard NATs.
- **E2EE**: SFrame (RFC 9605) so relays and SFUs forward what they cannot read; room key from
  the owner over Noise first, MLS (RFC 9420) later.
- **Synchronized playout**: clock sync over librats ping, a common playout delay so every viewer
  sees the same frame at the same moment.
- **Interop**: C API, `ratcast-send` / `ratcast-recv` CLI with MPEG-TS in/out, `rats://` URLs,
  a public netem benchmark against librist, SRT and libwebrtc.

## librats work

1. Priorities inside `Priority::Live`: audio > keyframe / base layer > deltas > enhancement layers.
2. Absolute deadlines when forwarding, so latency does not accumulate per hop.
3. Per-live-message delivered / expired feedback.
4. FEC across messages (small Opus packets) — or rely on Opus in-band FEC + RED.
5. A delay-sensitive realtime mode for BBR (or a GCC-style controller), compared on `bench_path`.
6. `send_many` with a shared refcounted buffer for fan-out.
7. Path MTU discovery above the fixed 1220-byte packets.
8. A clock-sync service.

## Media engine

| | Screen / window | Per-app audio | Camera |
|---|---|---|---|
| Windows | Windows.Graphics.Capture, DXGI Duplication fallback | WASAPI process loopback | Media Foundation |
| macOS | ScreenCaptureKit | ScreenCaptureKit (13+) | AVFoundation |
| Linux | PipeWire + xdg-desktop-portal, X11 fallback | PipeWire node capture | PipeWire / V4L2 |
| Android | MediaProjection | AudioPlaybackCapture | Camera2 |

- Audio: Opus (voice 24–48 kbit/s with DTX + FEC, music/stream 128–256 kbit/s stereo).
  WebRTC AudioProcessing (AEC3, AGC2, NS); RNNoise / DeepFilterNet as an option.
  Exclude the call's own playback from shared audio.
- Video: AV1 > HEVC > H.264. Hardware through FFmpeg (NVENC, AMF, QSV, VideoToolbox, VA-API /
  Vulkan Video, MediaCodec), low-latency settings. Software: SVT-AV1, dav1d, OpenH264.
- Zero-copy GPU path: captured texture → GPU colour conversion → hardware encoder; decoded
  texture → QRhi. Required for 1440p/4K60 games.
- Content-aware: encode films at their own frame rate, skip unchanged frames, 4:4:4 / screen-content
  mode for text.

## Scaling

- Voice: full mesh up to ~10–15; beyond that forward once and select the top-3 speakers by level.
- Video: simulcast / SVC + last-N.
- Stream 1→N: forwarding tree; SFU node past ~50 viewers or with poor uplinks.

## Repository layout

```
ratcast/
├── CMakeLists.txt        top level: options, module order
├── CMakePresets.json     debug / release / core (no Qt) / asan
├── cmake/                build helpers (compiler defaults, librats, testing)
├── external/librats/     submodule, developed together with ratcast
├── libratcast/           media transport library          → ratcast::ratcast
│   ├── include/ratcast/  public headers ("ratcast/<name>.h")
│   ├── src/              private sources
│   └── tests/
├── engine/               media I/O                         (stage 0–1)
├── client/               UI-agnostic client core           (stage 3)
├── app/                  Qt 6 / QML desktop application    → ratcast
├── server/               headless SFU / relay node         (stage 2)
├── tools/                ratcast-send / ratcast-recv, netem bench (stage 0–1)
└── docs/
```

Modules are added when their stage starts; each follows the libratcast shape
(`include/ratcast/<module>/`, `src/`, `tests/`).

Text chat lives in `client/` as one more room channel next to the call control channel: GossipSub
per room for delivery, librats storage for history when it is built.

Dependencies: system packages on Linux; a vcpkg manifest for Windows and macOS, added with the
first native dependency (Opus); Qt from the official installer or aqtinstall; FFmpeg as LGPL
shared libraries.

## Stages

| Stage | Result | Proves |
|---|---|---|
| 0. Prototype | CLI: screen capture on one OS → hardware encode via FFmpeg → live messages → decode → SDL window | glass-to-glass latency; whether live + FEC + BBR is enough as is |
| 1. libratcast core | framing, jitter buffer, A/V sync, rate control, keyframe requests, Opus, latency profiles, TS in/out, benchmark vs librist / SRT | the library holds its own |
| 2. One-to-many | tree, simulcast / SVC, absolute deadlines, SFrame, synchronized playout, SFU mode | the main use case |
| 3. App MVP | Qt UI, rooms and invites, voice with AEC / NS, screen / window / app-audio share, camera on Windows / Linux / macOS | the product |
| 4. Conferences | active speaker, last-N, grid, moderation, text chat | many participants |
| 5. Next | Android, cinema mode, HDR, polish | |

**Cinema mode**: when the streamer plays a video file, send its original H.264 / HEVC / AV1
packets paced by PTS instead of capturing and re-encoding the screen; transcode only the audio to
Opus. Original quality, no encoding cost, shared pause and seek.
