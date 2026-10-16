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

In the order 0.6 found them needed (measurements and reasons: `docs/bench.md`).

1. **Reordering is not loss for live data.** RACK's reordering adaptation is off while a live
   message is outstanding, so with live video always outstanding every reordered packet is a
   loss: 2% of packets arriving 25 ms early read as 15–25% loss, are sent twice and expire
   (25 fps, seconds of stalls); per-packet jitter, which reorders everything, plays nothing.
2. **A live flow's start.** (a) The initial window of 4 packets paces a fresh connection at
   4 packets per round trip × 2.77: a 50 KB keyframe takes ~190 ms to leave at 50 ms RTT and
   ~385 ms at 100 ms, past a 200 ms lifetime, and a live flow that then falls silent grows the
   rate once per keyframe request rather than once per round trip. 10 packets (RFC 9002) gets a
   keyframe through at 50 ms RTT; longer paths need the first keyframe to be smaller (rate
   control starting low) or to be allowed to arrive late (item 3).
   (b) Shedding by the pacing rate drops the frames behind a keyframe while that rate is still
   Startup's floor (the integration test's expired frame). (c) Startup's exit on a lifted round
   trip (an app-limited burst that "queues") fires on ±10 ms of jitter and leaves the estimate
   where the first keyframe left it: 2.7 s to the first picture. A joining scenario in
   `bench_path` to measure these on.
3. Per-live-message delivered / expired feedback: a sender learns that a keyframe expired at
   once, not from the receiver's next request a `keyframe_retry` later, and a joining viewer
   can be caught up instead of asked again (stage 1, item 3).
4. Priorities inside `Priority::Live`: audio > keyframe / base layer > deltas > enhancement layers.
5. Absolute deadlines when forwarding, so latency does not accumulate per hop.
6. FEC across messages (small Opus packets) — or rely on Opus in-band FEC + RED.
7. A delay-sensitive realtime mode for BBR (or a GCC-style controller), compared on `bench_path`.
8. `send_many` with a shared refcounted buffer for fan-out.
9. Path MTU discovery above the fixed 1220-byte packets.
10. A clock-sync service.
11. One copy fewer on the send path: `Node::send` copies the payload (`to_bytes()`) before
    `LiveSender` copies it again. Measured in 0.6: the reactor thread uses 0.8% of a core at
    8 Mbit/s, so it matters only for fan-out (with item 8).

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
├── CMakePresets.json     debug / release / core (no Qt) / asan / tsan
├── cmake/                build helpers (compiler defaults, librats, FFmpeg, PipeWire, testing)
├── external/librats/     submodule, developed together with ratcast
├── libratcast/           media transport library          → ratcast::ratcast
│   ├── include/ratcast/  public headers ("ratcast/<name>.h")
│   ├── src/              private sources
│   └── tests/
├── engine/               media I/O                         (stage 0–1)
├── client/               UI-agnostic client core: media pipelines now, rooms and chat later
├── app/                  Qt 6 / QML desktop application    → ratcast
├── server/               headless SFU / relay node         (stage 2)
├── tools/                ratcast-send / ratcast-recv
├── bench/                micro-benchmarks; netem.py: the tools across an emulated network
└── docs/
```

Modules are added when their stage starts; each follows the libratcast shape
(`include/ratcast/<module>/`, `src/`, `tests/`).

Text chat lives in `client/` as one more room channel next to the call control channel: GossipSub
per room for delivery, librats storage for history when it is built.

## Dependencies

| Dependency | For | Linux | Windows / macOS |
|---|---|---|---|
| FFmpeg ≥ 6 (avcodec, avformat, avutil, swscale) | encode, decode, demux | system | vcpkg, LGPL build we ship |
| PipeWire ≥ 0.3.40 | screen capture, audio | system | — |
| GIO (GLib) | xdg-desktop-portal over D-Bus | system | — |
| libdrm, GBM, EGL, Vulkan | DMA-BUF import, GPU colour conversion | system | — |
| SDL3 | windows of the CLI tools | system | vcpkg |
| Qt ≥ 6.5 with Quick | app | system | official installer / aqtinstall |
| Opus | audio (stage 1) | system (`libopus-dev`) | vcpkg |
| WebRTC AudioProcessing | AEC, AGC, NS (stage 3) | system | vcpkg or vendored |
| GoogleTest | tests | fetched by CMake | fetched by CMake |

A vcpkg manifest is added with the first dependency Windows or macOS needs. Each dependency is
optional at its module boundary: a missing one disables the backend that needs it, never the build.

Licensing: distribution FFmpeg builds often include GPL encoders (libx264, libx265). ratcast never
selects them, so it behaves the same against an LGPL FFmpeg; software fallbacks are SVT-AV1
(BSD) and OpenH264 (BSD).

## Stages

Stages are the roadmap; the current and next stage are broken into steps, later ones stay coarse
until we reach them. Every step ends with tests and leaves the code in its final place and shape:
stage 0 is a walking skeleton, not a throwaway prototype.

| Stage | Result | Proves |
|---|---|---|
| 0. Walking skeleton | screen → encoder → libratcast → librats → decoder → window, Linux first | glass-to-glass latency; whether live + FEC + BBR is enough as is |
| 1. libratcast core | rate control, loss recovery, audio and sync, latency profiles, netsim, benchmark vs librist / SRT, Windows and macOS backends | the library holds its own |
| 2. One-to-many | tree, simulcast / SVC, absolute deadlines, SFrame, synchronized playout, SFU mode | the main use case |
| 3. App MVP | Qt UI, rooms and invites, voice with AEC / NS, screen / window / app-audio share, camera on Windows / Linux / macOS | the product |
| 4. Conferences | active speaker, last-N, grid, moderation, text chat | many participants |
| 5. Next | Android, cinema mode, HDR, polish | |

### Stage 0 — walking skeleton

**0.0 Tooling.** ✅ `.clang-format`, CI on Linux / Windows / macOS for the libraries (app on Linux),
ASan + UBSan job. *Done:* every push builds and tests on three OSes.

**0.1 libratcast foundations.** ✅
- `Buffer`: refcounted immutable bytes from a pool, with slices — the one way media bytes travel.
- Time: `Timestamp` / `Duration` in microseconds on a monotonic clock; an injectable `Clock` for
  virtual-time tests.
- `EncodedFrame`: stream, frame number, capture time, key / delta, layer ids, references, payload.
- Wire format v0: versioned compact header (varints); `Result<T>` errors, no exceptions.

*Done:* header codec round-trips and rejects truncated or garbage input in tests; the send path
allocates nothing per frame beyond the payload.

**0.2 libratcast over librats.** ✅
- `MessageType::Media` in librats; libratcast's `MediaService` is a librats `Subsystem`.
- Reliable control: stream announce (codec, codec config, resolution, frame rate),
  subscribe / unsubscribe, keyframe request.
- Video frames as live messages, one frame per message, lifetime from the latency profile.
- Receiver: orders by frame number, drops frames whose references are missing, requests a keyframe
  (debounced) on a gap; per-stream stats.

*Done:* two nodes in one process exchange a synthetic stream; tests cover lost delta and key frames,
joining mid-stream, the sender leaving.

**0.3 engine: media model and codecs.** ✅
- `VideoFrame`: CPU planes (NV12, I420, BGRA) or a GPU handle (DMA-BUF now; D3D11, IOSurface,
  VA surface later) behind one refcounted type.
- Interfaces: `VideoSource`, `VideoEncoder` (runtime bitrate, forced keyframe), `VideoDecoder`.
- FFmpeg backend: hardware encoders first (NVENC, VAAPI, QSV, AMF, VideoToolbox), then SVT-AV1 /
  OpenH264; low-latency settings (no B-frames, ~1-frame VBV); decoding with hwaccel when available.
- Pattern source: synthetic frames with the capture time burned in — for tests and latency.

*Done:* encode → decode round trip for every encoder present; bitrate changes take effect.

Notes from 0.3: hardware encoders are wired up but untested (no GPU in the dev VM) — check
NVENC / QSV / AMF / VideoToolbox, in-band parameter sets and dynamic bitrate on real machines.
Software encoders cannot change bitrate in flight through FFmpeg and are reopened (a keyframe;
~30-50 ms for SVT-AV1 at 1080p), so rate control must change rate sparingly — or reopen in the
background. SVT-AV1 at 1080p60 takes ~8 ms a picture here, libaom ~19 ms (too slow for 60 fps).
Hardware decoding (hwaccel) is not in yet: decode is software (dav1d, FFmpeg's H.264 / HEVC).

**0.4 engine: Linux capture.** ✅ (VAAPI and DMA-BUF moved to stage 1)
- xdg-desktop-portal ScreenCast over GIO: screen or window, restore token so the picker is shown
  once; a connection of its own per cast, so the portal closes the session even after a crash.
- PipeWire stream: BGRx in shared memory, crop metadata, renegotiation on resize; the capture ends
  when the node goes (the registry says so), pictures held to a maximum rate.
- BGRA → NV12 / I420 on the CPU, in the same pass that empties the buffer; `ColorSpace` (BT.709 /
  BT.601, limited / full) on `VideoFrame`, signalled by encoders, read back by decoders.
- File source (FFmpeg demux + decode, libswscale for other formats) for machines without capture,
  and the base of cinema mode.

*Done:* captures a GNOME Wayland desktop and a single window; survives resize and the window
closing.

Notes from 0.4: the dev VM (VMware, vmwgfx) has no VA-API driver, so VAAPI encoding and the
DMA-BUF path it needs cannot run here; they move to stage 1 item 8 with the other GPU paths, to be
built and tested on an Intel or AMD machine. Meanwhile capture asks for shared memory, which the
compositor fills from the GPU itself. The pointer is drawn in by the compositor (embedded mode),
which works the same for both memory kinds; metadata mode pays off once the cursor travels as a
stream of its own. Tested against a stand-in PipeWire node and a fake portal on a private bus, and
against real mutter through `org.gnome.Mutter.ScreenCast`. Conversion: 1.0 ms for 1080p, 5.2 ms
for 4K (1.8x and 1.6x faster than libswscale). Mutter sends a picture only when the screen
changes, so the sender in 0.5 must keep the last picture to encode a requested keyframe from.
PipeWire 1.6 leaks a stream destroyed while it is still connecting; teardown waits that out.

**0.5 Tools.** ✅ on one host (two machines: by hand) — `ratcast-send` (source → encoder → MediaService) and `ratcast-recv`
(MediaService → decoder → SDL3 window, or `--headless`); connect by address or by peer id (DHT,
`--announce` / `--peer`); `--source screen|window|file|pattern`, `--codec`, `--bitrate`, `--fps`,
`--latency`; stats once a second, with each viewer's path (RTT, estimate, loss, expired) on the
sender and capture-to-picture latency on the receiver when the pattern's stamps can be read.
*Done:* one machine streams its desktop to another over a real network.

Notes from 0.5: the pipelines are the `client` module's (`VideoPublisher`, `VideoPlayer`), so the
app reuses them; the tools only parse options and print. The publisher keeps only the newest
picture (skips, never queues), repeats the last one when a keyframe is wanted from a still screen,
and starts a new encoder on a size change with frame numbers carrying on. `request_keyframe()` on a
session now breaks the stream until the keyframe arrives and repeats the request (a sender ignores
one too soon after its last keyframe). Loopback, release build, SVT-AV1 + dav1d at 8 Mbit/s:
capture to decoded picture 14.7 ms at 720p60, 22 ms at 1080p60 (encode 8.4 / 10.3 ms, decode
3.2 / 3.5 ms), no losses. A real second machine and the portal dialog are still to be tried by
hand. UBSan found undefined left shifts in librats' curve25519 (negative values, int overflow);
fixed there, librats' 1035 tests pass under it.

**0.6 Baseline.** ✅ (one host; two hosts by hand) Glass-to-glass latency with the pattern
source, CPU, bitrate, behaviour under `tc netem` loss, delay, jitter, reordering and caps, written
to `docs/bench.md`. Also the first seconds of a stream: the frame expiring after a large keyframe.
*Done:* we know which librats items to do first.

Notes from 0.6: `bench/netem.py` runs the tools in two network namespaces of an unprivileged user
namespace (no root), the veth between them shaped per direction; `PlaybackMeter` (client) measures
what a viewer sees — the wait for the first picture, stalls, latency percentiles — and the tools
print it, with CPU, as a `summary` line; the pattern is stamped with the time of day, so two hosts
with synchronised clocks measure latency too. On one host, 1080p60 AV1 at 8 Mbit/s: 18 ms capture
to decoded picture (p50), 21 ms to the window; SVT-AV1 takes a core, dav1d a quarter, librats under
1%. Clean and lossy paths (1–5%, bursts) play at one-way delay + 18 ms, p95 +40–60 ms from
repairs, a few short stalls. Three problems, all in the transport's first seconds or its loss
detection: joining takes 0.5 s at 50 ms RTT and 2 s at 200 ms (the initial window paces a
keyframe out slower than its lifetime; the frame behind it is shed by Startup's floor rate — the
0.5 integration test's expired frame); 2% reordering reads as 20% loss and leaves 26 fps; jitter
ends BBR's Startup early. Hence the new order of "librats work" above.

### Stage 1 — libratcast core

1. **Rate control:** encoder bitrate from `PathStats` (fast down on expiring frames or a growing
   queue, careful up), starting low so that the first keyframe is small, then a resolution /
   frame-rate ladder.
2. **librats:** live sub-priorities, per-message delivery feedback, absolute deadlines.
3. **Loss recovery and joining:** reference invalidation / LTR where encoders support it,
   intra-refresh for `game`, keyframe-request aggregation; a joining viewer's keyframe allowed
   to arrive late (with delivery feedback) and the viewer caught up, instead of asked again.
4. **Audio:** PipeWire capture and playback, Opus (FEC, DTX), adaptive jitter buffer, A/V sync.
5. **Latency profiles** `call` / `game` / `cinema` end to end.
6. **netsim:** userspace impairment proxy (delay, jitter, loss, bursts, bandwidth caps; no root,
   any OS) with scripted scenarios in CI; `bench/netem.py` covers Linux meanwhile.
7. **Interop and benchmark:** C API, MPEG-TS in / out in the tools, `rats://` URLs, comparison
   with librist and SRT on netsim scenarios.
8. **GPU paths and the other platforms:** Linux DMA-BUF capture → VAAPI encoding (colour
   conversion on the GPU), hardware decoding; Windows.Graphics.Capture + D3D11 → hardware encoder;
   ScreenCaptureKit + VideoToolbox — the engine is cross-platform before the app is built on it.
