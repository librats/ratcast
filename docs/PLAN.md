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
| Scope, first stages | Calls and streams. Text chat is designed for (room protocol has a place for it), built later |
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
├── engine/               media I/O
├── client/               UI-agnostic client core: media pipelines now, rooms and chat later
├── app/                  Qt 6 / QML desktop application    → ratcast
├── server/               headless SFU / relay node         (stage 5)
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
| PipeWire ≥ 0.3.50 | screen capture, audio | system | — |
| GIO (GLib) | xdg-desktop-portal over D-Bus | system | — |
| libdrm, GBM, EGL, Vulkan | DMA-BUF import, GPU colour conversion (stage 4) | system | — |
| SDL3 | windows of the CLI tools | system | vcpkg |
| Qt ≥ 6.5 with Quick | app | system | official installer / aqtinstall |
| libopus ≥ 1.3 | audio | system (`libopus-dev`) | vcpkg |
| WebRTC AudioProcessing | AEC, AGC, NS (stage 6) | system | vcpkg or vendored |
| GoogleTest | tests | fetched by CMake | fetched by CMake |

A vcpkg manifest is added with the first dependency Windows or macOS needs. Each dependency is
optional at its module boundary: a missing one disables the backend that needs it, never the build.

Licensing: distribution FFmpeg builds often include GPL encoders (libx264, libx265). ratcast never
selects them, so it behaves the same against an LGPL FFmpeg; software fallbacks are SVT-AV1
(BSD) and OpenH264 (BSD).

## Roadmap

Two tracks:

- **Stages** — ratcast's roadmap. Each ends in a result a viewer or a benchmark can see, and has
  a *done when* that says how it is checked. Steps are numbered within their stage (`1.2`).
- **librats track** — the transport work ratcast needs, done in the submodule. It is a list of
  its own, in priority order, with stable ids (`L2`), so that it never holds the stages up as a
  whole.

A step names the librats items it depends on: **needs** — the step's *done when* waits for it;
**better with** — the step works without it, measurably worse on some paths. A step goes ahead
before its needs are met, with the gap measured in `docs/bench.md`; the librats item is taken up
when it is first in the librats order or when nothing else in the stage can move without it.

The current stage and the next are broken into steps; later ones stay coarse until reached.
Every step ends with tests (and benchmarks when it moves a number) and leaves the code in its
final place and shape: stage 0 is a walking skeleton, not a throwaway prototype.

| Stage | Result | librats needs |
|---|---|---|
| **0. Skeleton** ✅ | screen → encoder → libratcast → librats → decoder → window, on Linux; latency and behaviour on emulated networks measured | L1 ✅ |
| **1. A stream that holds** ✅ | one video stream to a few viewers plays on every emulated path: rate control, fast joining, smooth playback on lossy paths | L2 (c) ✅, L3 ✅; L2 (a, b) put off |
| **2. Sound** ✅ | audio, A/V sync; latency profiles put off to stages 5 and 6 | L4 ✅ |
| **3. Standalone library** ← now | MPEG-TS in and out, `rats://` URLs, a benchmark against librist and SRT, C API | — |
| **4. Platforms** | GPU paths and hardware codecs; Windows and macOS capture; netsim | — |
| **5. Watch together** | one-to-many: forwarding tree, simulcast / SVC, SFU, SFrame E2EE, synchronized playout | L5 |
| **6. App** | Qt app MVP: rooms and invites, voice with AEC / NS, screen / window / app-audio share, camera, on Windows / Linux / macOS | — |
| **7. Conferences** | active speaker, last-N, grid, moderation, text chat | — |
| **8. Beyond** | Android, cinema mode, HDR, polish | — |

### Stage 0 — Skeleton ✅

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

Notes from 0.3: hardware encoders are wired up but untested (no GPU in the dev VM) — checking
NVENC / QSV / AMF / VideoToolbox, in-band parameter sets and dynamic bitrate on real machines is
4.2. Software encoders cannot change bitrate in flight through FFmpeg and are reopened, with a
keyframe; since 1.1 the new encoder is opened alongside the old one, so a change no longer stops
the stream. SVT-AV1 at 1080p60 takes ~8 ms a picture here, libaom ~19 ms (too slow for 60 fps).
Hardware decoding (hwaccel) is not in yet: decode is software (dav1d, FFmpeg's H.264 / HEVC).

**0.4 engine: Linux capture.** ✅ (VAAPI and DMA-BUF moved to 4.1)
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
DMA-BUF path it needs cannot run here; they move to 4.1 with the other GPU paths, to be built and
tested on an Intel or AMD machine. Meanwhile capture asks for shared memory, which the
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
ends BBR's Startup early. Hence the order of the librats track below.

### Stage 1 — A stream that holds ✅

One sender, one video stream, a few viewers. The stream adapts to the path, starts quickly and
gets over losses without the viewer noticing — on every path `bench/netem.py` emulates. Audio,
many viewers and other platforms are later stages.

*Done when* `bench/netem.py --repeat 3` shows:
- at least 55 fps in every scenario but `cap-drop`, which is back to full rate within 2 s of
  each change of the path;
- the first picture within about four round trips plus 150 ms (now 0.7 s at 100 ms RTT,
  1.45 s at 200 ms), and no stall right after it (now 0.1–0.2 s on most paths);
- no scenario playing worse with rate control than at a fixed bitrate (✅ since L2 (c): `jitter`,
  `reorder` and `reorder-2` play at 59–60 fps like the clean paths).

Measured whole after 1.1–1.3 (`docs/bench.md`, "Stage 1, measured whole"), and closed with two
gaps put off rather than chased now:
- 58–60 fps everywhere ✅; `cap-drop` back at full rate within a second when the path widens ✅,
  but about three seconds after it narrows, and `reorder` loses a few frames at bitrate changes
  that a fixed bitrate does not (58 against 60 fps). Both are keyframes: each change of a
  software encoder's bitrate is one. That is 1.1's leftover "fewer keyframes", for when a
  measurement or a user asks;
- the first picture within four round trips + 150 ms with no stall after it up to 50 ms RTT ✅
  (328 ms at 50); at 100 and 200 ms 616 and 1178 ms, then 0.15 and 0.4 s of stall — librats
  L2 (a, b), put off: joining faster is not urgent.

Order it was done in: 1.1, 1.3, 1.2 with L3.

**1.1 Rate control.** ✅ The encoder's bitrate from the viewers' `PathStats`: fast down on expiring
frames or a growing queue, careful up, starting low so that the first keyframe is small. Left:
- ✅ the start's false cut: frames expiring while the transport is in Startup are no overuse;
- ✅ a narrowing path in one cut, by what it delivered rather than by BBR's lagging estimate;
- fewer keyframes: every bitrate change of a software encoder is a keyframe (7 in a 15 s run
  against 3 at a fixed bitrate, 15 on `cap-6`) — once ramped, change less often and in larger
  steps; encoders that change in flight (hardware, 4.2) need no keyframe at all;
- a resolution / frame-rate ladder for paths too narrow for the picture at any bitrate.

librats: **needs** L2 (c) ✅ (on `jitter` the estimate rate control follows was what Startup
left, 1–2 Mbit/s on a 1 Gbit/s path).

Notes from 1.1: `RateController` (libratcast) turns each viewer's path samples — librats'
`on_peer_path`, every 100 ms — into one bitrate per stream, the slowest viewer's: start at
2 Mbit/s, ×1.5 a second up, cut to 85% of what a full path carries (busy two samples running,
its estimate settled) on expired frames, a full queue or a quarter-lifetime wait, careful +8%
steps back towards where it filled up. The round trip is no signal (jitter alone lifts it). The
publisher hands the bitrate to the encoder; encoders FFmpeg cannot change in flight open a new
one alongside and switch with a keyframe (SVT-AV1 at 1080p: a 25–31 ms picture instead of a
55 ms stop). netem: `cap-6` from 30 to 59 fps, joining a third faster; it follows BBR's estimate
wherever that goes, so `jitter` and `reorder-2` played at 1.5–3.5 Mbit/s with more stalls than at
a fixed 8 — until L2 (c), below. Two streams to one viewer (camera and screen) each take the
whole path for now: they share it in stage 6.

After L2 (c) (librats as of 2026-10-20; `bench/netem.py --repeat 3`, rate-controlled, before →
after): `jitter` 53 → 60 fps, 1.7 → 5.7 Mbit/s over the run, 9 → 2 cuts, p95 156 → 88 ms,
stalls 2.6 → 0.2 s; `reorder` 54 → 59 fps, 1.4 → 5.3 Mbit/s, stalls 2.6 → 0.5 s; `reorder-2`
58 → 60 fps, 3.4 → 5.6 Mbit/s, no stalls. The jittery paths now ramp and hold like the clean
ones (5.6–5.7 Mbit/s over a 15 s run, ramp included); the clean and lossy rows are unchanged.

Then (`docs/bench.md`, "The start and a narrowing path"): the start's cut came from frames
expiring while BBR was in Startup, pacing at 2.74× an estimate that is only how far it got; a
sample pacing at more than twice its estimate now counts for nothing but a full queue, and every
path but the narrow ones ramps to 8 Mbit/s in four seconds with no cut (5.9–6.3 Mbit/s over a
run). A cut goes to 85% of the lesser of BBR's estimate and what the path delivered between two
busy samples: after `cap-drop`'s fall to 4 Mbit/s the estimate took 3.5 s to come down, the
delivery said 4 at once — 55 fps and 1.5 s of stalls, from 50 fps and 2.9 s. The cut's new
encoder had been hiding the start's expired frame with its keyframe; now an expiry on a
subscriber's path is a keyframe needed at once (and a keyframe asked for while a new encoder
opens comes with it), which leaves a 0.1–0.2 s stall after the first picture — 1.2's.

**1.2 Joining.** A joining viewer's keyframe is allowed to arrive late, and the viewer is caught
up rather than asked again; keyframe requests from several viewers in one window are answered
with one keyframe. *Done:* the first-picture target above at every distance. Done in ratcast with
L3 ✅; left, put off: L2 (a, b), without which the target is out of reach from ~150 ms RTT up
(below).
librats: **needs** L3 ✅ (the sender learns the keyframe is on its way or expired) and, for the
long paths, L2 (a, b) (initial window, shedding by Startup's rate).

Notes from 1.2: where the time went (one run each, timestamps on both ends): the viewer
subscribes two round trips after it dials (handshake, then the announce), the sender hears of it
half a round trip later — and its first keyframe, ~25 KB, cannot leave in the 200 ms − one-way it
had behind a fresh connection's 4-packet window: it expired, the next met a window only a little
wider, and at 200 ms RTT six went before one arrived (1.45–1.7 s to the first picture); the frames
behind the one that did expired too, which is the stall after it. Now a subscriber's first
keyframe goes with four round trips more than the stream's lifetime, and every frame after it
with what the one before got less half the time since — each due after the one before, as EDF
sends them; ms rounding made a shared deadline reorder them — so the viewer gets the keyframe
late and plays the frames behind it catching up (`MediaSession`, "Joining"). Each frame is
tagged (L3): the sender hears at once that the joining keyframe arrived (joined) or was given up
(sent again), ignores the receiver's requests while it is on its way, and asks for a keyframe the
moment a frame to a subscriber is given up on — no longer from the path's expiry count, which
says how many, not which, and comes up to a sample later. A frame given up on behind the
joining keyframe brings the next one at once: it goes right behind it, as late. The receiver
waits for its first keyframe however late (`FrameTracker`; before, frames behind a gap it left
were dropped after the lifetime, and a keyframe repaired late on a long lossy path came to
nothing). One `netem` run each (6 s, `docs/bench.md`), before → after: first picture `rtt-50`
328 → 328 ms, no stall after it (was 0.1 s); `rtt-100` 681 → 601–621 ms, stall after it 113 →
136 ms; `rtt-200` 1448 → 1164–1188 ms, stall 191 → ~400 ms; `loss-2` 330 → 323–384 ms. What is left on the long
paths is librats' (L2 (a, b)): the 4-packet window holds the keyframe ~3 round trips, and while
it does, BBR's Startup paces at its floor (~0.5 Mbit/s at 200 ms), by which the frames behind it
are shed as unable to leave in time — up to 8 of them at 200 ms, and the keyframe after.

**1.3 Smooth playback on lossy paths.** ✅ The stalls on lossy paths (3–8 in 15 s, `burst` the
most) are frames that arrived, late: a packet FEC did not rebuild is sent again, a round trip
and its detection later (~+70 ms at 50 ms RTT), and the frames behind it, there already, wait
for it in decode order. Frames actually lost are 1–3 a run, mostly at the start, and since 1.1
an expiry on a subscriber's path is a keyframe at once. So the player shows pictures on a
schedule — capture time on this clock plus a delay that covers how late frames have been
arriving lately, growing at once to a late frame, shrinking slowly back — up to a limit
(the stream's lifetime: no frame comes later), which 2.3's profiles set. *Done:* no more than
one stall in 15 s on `loss-*` and `burst` past the start, for ~60–70 ms more latency there, and
~20 ms on clean paths for even motion (below). If repairs still come too late for it, more FEC
for live messages (fewer repairs that wait a round trip) is librats' to offer. Left:
- SVT-AV1's low-delay structure makes every fourth frame ~3× the size of the others and ~12 ms
  slower to encode, so frames reach even a local viewer in clumps every 66 ms — motion that
  judders, shown as it comes; the buffer evens it out for ~20 ms on every path. An encoder
  setting without the structure (`hierarchical-levels=0` changes nothing) or a hardware encoder
  (4.2) would let it shrink to nothing on clean paths.
Reference invalidation, LTR and intra-refresh move to 4.2: neither SVT-AV1 nor libaom offers
them through FFmpeg, and lost frames are few; intra-refresh on hardware encoders is worth more
against the keyframes of 1.1 (`cap-6`).

Notes from 1.3: `PlayoutSchedule` (client) times each picture at its capture time on this clock
— arrival less capture time, least over 10–20 s, is the clocks' offset plus the path — plus a
delay; `VideoPlayer` decodes at once and hands pictures on when due (`--buffer MS` in
`ratcast-recv`, 0 for the old way). Late frames come in runs, up to one on time; the delay covers
the second latest run of ten seconds, not the latest (one run is a stall whatever the delay, and
the worst moment of ten seconds is not worth every picture's latency), grows at once and shrinks
at 5% speed. Runs within 200 ms of a keyframe do not count: it took long to send, and at a new
bitrate the encoder opening alongside slows the one at work for a few frames before it, the new
one is slow for a few after — counted, a ramp held the delay at 50 ms on a clean path. One
`netem` run each: `burst` 8 → 1 stalls at 47 → 125 ms (p50), `loss-2` 3 → 2 (both at the start)
at 49 → 111 ms, `rtt-50` 48 → 66 ms, `local` 16 → 34 ms.

### Stage 2 — Sound

Audio next to the video, in sync, and the latency profile as the one knob over both.
*Done when* the pattern source's audio and video play in sync (within 40 ms) across the netem
scenarios, audio stays uninterrupted under 2% loss and next to a keyframe's burst, and the tools
take `--latency call|game|cinema`. The profiles were put off (2.3): the stage closed on the rest.

- **2.1 Audio:** ✅ PipeWire capture and playback, Opus (voice with DTX + FEC, music stereo),
  adaptive jitter buffer. librats: **needs** L4 (audio must not wait behind a keyframe);
  L6 only if Opus in-band FEC + RED turn out not to be enough. In steps:
  - (a) ✅ Opus in the engine: `AudioFrame`, `AudioEncoder` / `AudioDecoder` over libopus — any
    block size in, frames out; DTX that sends nothing in silence; in-band FEC; a gap filled by
    the next frame's FEC or by concealment. ~0.1 ms a frame to encode (`docs/bench.md`). FEC
    brings a single lost frame back nearly whole; of two in a row the first is concealed and
    the second, from FEC after concealment, comes out at a quarter of its level — bursts are
    what RED or L6 would be for, if a measurement asks;
  - (b) ✅ sound in and out: `AudioSource` and `AudioOutput` (pulled by the device, told when
    the sound will be heard), PipeWire capture — a microphone, the whole of what the computer
    plays, or one node — and playback, the tone source. The PipeWire plumbing video capture had
    is shared now (`pipewire::StreamThread`);
  - (c) ✅ audio over the session: `MediaSession` sends an audio stream's frames with neither
    keyframe requests nor joining nor outcomes, and hands them on as they arrive
    (`FrameTracker` without `in_order`); `JitterBuffer` (libratcast `stream/`, codec-agnostic,
    tested in virtual time) plays them by capture time behind a delay that covers 97% of their
    lateness plus a frame — so the frame after a lost one is there for its FEC — and grows where
    sound is made up anyway, shrinks in silence; L4 sends sound ahead of a keyframe;
  - (d) ✅ client and tools: `AudioPublisher` (source → encoder thread → session) and
    `AudioPlayer` (session → jitter buffer → decoded on the output's own pull), `VirtualOutput`
    (sound played in real time to nobody: headless viewers, tests), `--audio` in `ratcast-send`
    (tone, mic, system) and `ratcast-recv` (speaker, virtual, off), `bench/netem.py --audio`.
    On single runs (`docs/bench.md`): 56 ms from capture to speaker on a clean path; 2% loss
    without a break (FEC brings back 10 of 11 frames); keyframes every 2 s next to the sound
    cost nothing where the path has room. L4 halves the harm where the stream fills the queue:
    `cap-6` 180 → 100 ms at the median, lost frames 16 → 8. A path narrower than the stream
    still breaks the sound for a moment, in the bottleneck's queue, which no rank overtakes:
    that is rate control's margin.
- **2.2 A/V sync:** ✅ a common playout clock per sender (`PlayoutClock`, client), shared by its
  audio and video players. The pictures are held until the sound captured with them is heard.
  Where the pictures need longer (decoding, frames sent again on lossy paths), the sound waits
  for them: a floor under the jitter buffer's target, reached by making up a frame every 100 ms
  after a second short of it (`JitterBuffer::set_min_delay()`). `ratcast-recv --no-sync` only
  measures. On single runs (`docs/bench.md`): apart ≤ 2 ms at the median and ≤ 16 ms at p95 on
  `local`, `jitter`, `loss-2` and `burst`, against 24 ms (clean) and 44 ms at p95 (2% loss) not
  in step. The 1% beyond are pictures late on their own, as late without it. The cost: on a
  clean path the pictures wait for the sound's 56 ms, which 2.3's `call` and `game` profiles
  start from.
- **2.3 Latency profiles** — put off. Every knob a profile would set already follows the path
  (the jitter buffer, the playout schedule, rate control, lip sync), and the one left, the
  frames' lifetime, is `--latency MS`; a profile is the user's choice, which waits for the app.
  `call` / `game` go to stage 6, `cinema` to stage 5, where it is real work (seconds of buffer,
  held encoded rather than decoded). Left with them, as the stage's closing run shows
  (`docs/bench.md`, "Stage 2, measured whole"): lip sync makes the pictures wait for the sound,
  so the sound's latency is theirs — 30 → 57 ms on `local`, 80 → 209 ms on `cap-drop`. Two
  things to take out of it: the frame for FEC kept on clean paths (keep it only where the path
  loses packets), and a jitter-buffer delay that stays wherever a spike pushed it while there is
  no silence to cut (a tighter drop threshold, or time-stretching). Meanwhile `--no-sync` plays
  the pictures at stage 1's latencies.

Closed with all 16 scenarios measured three times (`docs/bench.md`): the pictures within 1 ms of
the sound at the median and 34 ms at p95 at worst (`cap-6`); the sound without a break under
every loss and next to keyframes, one break each where the path cannot carry the lifetime or the
stream (`rtt-200`, `cap-drop`).

### Stage 3 — Standalone library

libratcast usable and measured on its own, against what it is meant to replace.
*Done when* `docs/bench.md` has the comparison with librist and SRT on the same scenarios.

librist and SRT carry an MPEG-TS byte stream, so that comes first: the comparison is the same TS
across the same paths. In steps:

- **3.1 MPEG-TS through ratcast:**
  - (a) ✅ libratcast: data streams (`MediaKind::Data`, `Codec::MpegTs`), carried as sound is —
    every message stands alone, no keyframes, handed on as it arrives — and a `DeliveryBuffer`
    (`stream/`, tested in virtual time) that hands them on in order at a fixed delay past the
    fastest, skipping what did not come in time: what SRT calls TSBPD with too-late drop, and
    librist its buffer;
  - (b) the tools: `ratcast-send --input` and `ratcast-recv --output` for TS over UDP, files and
    pipes; `rats://` URLs for where a stream is.
- **3.2 Benchmark** against librist and SRT (libwebrtc if practical). librats: L9 optional.
  - (a) the harness: a stamped constant-bitrate TS probe (sender and meter: latency, loss,
    continuity), librist's and SRT's own tools built from pinned sources without root,
    `bench/netem.py` running each of the three over the same scenarios;
  - (b) the comparison, in `docs/bench.md`.
- **3.3 C API.**

netsim, the userspace impairment proxy (delay, jitter, loss, bursts, bandwidth caps; no root, any
OS; scripted scenarios in CI), moved to stage 4: it is for the other platforms, and
`bench/netem.py` covers Linux.

### Stage 4 — Platforms

The engine on every desktop OS, with the GPU paths high resolutions need, before the app is built
on it. *Done when* `ratcast-send` / `ratcast-recv` run on Linux, Windows and macOS with hardware
encoding, and a 1440p60 game is captured and encoded without a CPU copy.

- **4.1 Linux GPU path:** DMA-BUF capture → VAAPI encoding with colour conversion on the GPU,
  hardware decoding (on an Intel or AMD machine; the dev VM has no VA-API).
- **4.2 Hardware encoders on real machines:** NVENC, QSV, AMF, VideoToolbox — in-band parameter
  sets, bitrate changed in flight (no keyframe per change, 1.1), intra-refresh in place of
  keyframes and LTR / reference invalidation where the encoder has them (from 1.3).
- **4.3 Windows:** Windows.Graphics.Capture + D3D11 → hardware encoder.
- **4.4 macOS:** ScreenCaptureKit + VideoToolbox.
- **4.5 netsim** (from stage 3): a userspace impairment proxy (delay, jitter, loss, bursts,
  bandwidth caps; no root, any OS) with scripted scenarios in CI, for where `bench/netem.py`
  cannot run.

### Stage 5 — Watch together

The main use case: one person streams, many watch. Coarse until reached.

- Capacity-aware forwarding tree: viewers with spare uplink re-forward without transcoding.
  librats: **needs** L5.
- Simulcast / SVC layer selection per receiver.
- SFU mode and the `server` node for big rooms and hard NATs. librats: **better with** L8, L11.
- SFrame E2EE, room key from the owner over Noise (MLS later).
- Synchronized playout: clock sync, a common playout delay. librats: L10 optional.
- The `cinema` latency profile (from 2.3): seconds of buffer for retransmission to repair
  anything, held as encoded frames (decoded 1080p is ~3 MB a picture), lip sync over it.

### Stage 6 — App

Qt 6 / QML MVP on Windows, Linux and macOS: rooms and invites, voice with AEC / NS (WebRTC
AudioProcessing), screen / window / app-audio share, camera. Camera and screen to one viewer
share the path (each stream takes the whole path today, 1.1). Latency profiles `call` / `game`
as the user's choice (from 2.3): lifetimes, jitter-buffer depth, keyframe policy, encoder
settings, whether a game's pictures wait for its sound; the sound's frame for FEC only on lossy
paths.

### Stage 7 — Conferences

Active speaker, last-N, grid, moderation, text chat.

### Stage 8 — Beyond

Android, cinema mode, HDR, polish.

## librats track

Transport work for ratcast, done in the submodule (its own `CLAUDE.md` and `ARCHITECTURE.md`,
committed in its repository). Ids are stable — stage steps and `docs/bench.md` refer to them —
and the list is in priority order:

- **needed** — a ratcast stage cannot be done without it; in the order the stages come;
- **improves** — ratcast works without it, measurably worse on some paths or at some scale;
- **optional** — there is an alternative, or no measurement asks for it yet.

| # | id | item | tier | for | status |
|---:|---|---|---|---|---|
| | L1 | Reordering is not loss for live data | needed | 0.6 | ✅ |
| | L2 (c) | BBR on a jittery path | needed | 1.1 | ✅ |
| | L3 | Per-message delivered / expired feedback | needed | 1.2 | ✅ |
| | L4 | Priorities inside `Priority::Live` | needed | 2.1 | ✅ |
| 1 | L2 (a, b) | A live flow's start: initial window, shedding | needed | 1.2 (long paths) | put off |
| 2 | L5 | Absolute deadlines when forwarding | needed | stage 5 (tree) | |
| 3 | L8 | `send_many` with a shared buffer | improves | stage 5 (SFU) | |
| 4 | L11 | One copy fewer on the send path | improves | with L8 | |
| 5 | L6 | FEC across messages | optional | 2.1 | if Opus FEC + RED fall short |
| 6 | L9 | Path MTU discovery | optional | 3.4, 4K | |
| 7 | L10 | A clock-sync service | optional | stage 5 (playout) | libratcast can do it over ping |
| 8 | L7 | A delay-sensitive realtime mode | optional | — | mostly done with L2 (c) |
| 9 | L12 | Live outcomes and ranks in the C ABI | optional | other bindings | when one needs them |

Where it stands after L1, L2 (c) and L3: the steady state holds — one-way delay + ~18 ms on a
clean path, 59–60 fps under 1–5% loss, bursts, jitter and reordering, and BBR's estimate follows
a jittery path as it does a clean one, so rate control (1.1) ramps and holds there too. What
every viewer still meets is the first second, which ratcast has taken as far as it can (1.2: the
joining keyframe arrives late instead of being sent again, 1.45 → 1.2 s at 200 ms RTT): the
keyframe behind a 4-packet initial window and the frames behind it shed by Startup's floor rate
— 0.6 s to the first picture at 100 ms RTT, 1.2 s at 200 ms, and a stall of up to 0.4 s after
it. That is L2 (a, b), put off with stage 1 closed (joining faster is not urgent). L4, for
stage 2's audio, is done: live messages leave by rank, then deadline.

**L1 — Reordering is not loss for live data.** ✅ RACK's reordering adaptation was off while a
live message was outstanding, so with live video always outstanding every reordered packet was a
loss, sent twice and reported to BBR, which cut its estimate below the stream: 2% of packets
arriving 25 ms early read as 15–25% loss (26 fps, seconds of stalls); per-packet jitter, which
reorders everything, played nothing. Now live data waits out a path's learnt reordering like bulk
does, the window may grow up to a probe timeout rather than a round trip (jitter larger than the
round trip itself), and FEC sizes its repairs knowing a loss is found that much later. Found on
the way: a packet number was sent in one byte, so one overtaken by more than 127 others (a
keyframe's burst past one held back) was read 256 off — lost under Noise, and in plaintext taken
for a later packet whose data then never came; now never under two bytes. Left, neither blocking
(`docs/bench.md`): per-packet jitter several times the base round trip (netem's `reorder`) —
BBR's window, built on a minimum round trip of a few ms, cannot hold a keyframe (done in L2 (c));
and 4K60 at
80% of a link that reorders a few packets in a hundred, 1–3% of frames late, since a real loss
now waits out the window too — rate control's headroom (1.1) covers it.

**L2 — A live flow's start.** Three parts:
- (c) ✅ *BBR on a jittery path.* Startup's exit on a lifted round trip fired on ±10 ms of
  jitter and left the estimate where the first keyframe left it: 2.7 s to the first picture at a
  fixed bitrate, 1–2 Mbit/s for the whole run with rate control. Now a standing queue in Startup,
  or a burst that queued in a probe, is checked by padding up to the pacing rate before it is
  believed; the window is sized over the round trip the path shows at load (Little's law), not
  over one lucky minimum; a probe ends only on a burst later than twice the path's own ack
  lateness; random loss on a path that holds packets back is no longer read as a full queue; a
  late packet behind many gaps is taken in rather than dropped as a duplicate. netem: `jitter`,
  `reorder` and `reorder-2` at 59–60 fps and 5.3–5.7 Mbit/s (1.1's notes).
- (a) *Needed by 1.2 on long paths.* The initial window of 4 packets paces a fresh connection at
  4 packets per round trip × 2.77: a keyframe leaves slower than its lifetime from ~100 ms RTT
  up (32 frames expire at 200 ms before the first picture), and a live flow that then falls
  silent grows the rate once per keyframe request rather than once per round trip. 10 packets
  (RFC 9002) gets a keyframe through at 50 ms RTT; longer paths need the first keyframe to be
  smaller (rate control starting low, ✅) or to be allowed to arrive late (1.2 ✅, with L3) — and
  then it still takes ~3 round trips through the window: 1.2 s to the first picture at 200 ms
  RTT, where the target is ~0.95 s.
- (b) *Needed by 1.2 on long paths.* Shedding by the pacing rate drops the frames behind a
  keyframe while that rate is still Startup's floor (~0.5 Mbit/s at 200 ms RTT): with the joining
  keyframe let arrive late (1.2), up to 8 frames behind it are shed, and the keyframe that has to
  follow is a 0.4 s stall after the first picture.

The start's expired frames were also a false rate cut at the start of nearly every netem run,
which 1.1 no longer takes them for. A joining scenario in `bench_path` to measure (a) and (b) on,
in virtual time, when they are taken up.

**L3 — Per-live-message delivered / expired feedback.** ✅ *Needed by 1.2.* A sender learns that a
keyframe expired at once, not from the receiver's next request a `keyframe_retry` later, and a
joining viewer can be caught up instead of asked again. `SendOptions::tag`; the sender hears
`PeerNetwork::on_live_outcome(type, (peer, tag, delivered))` the moment the message is settled —
acknowledged (or FEC-complete) on UDP, sealed onto the stream on TCP, or given up on, shed
included — recorded by `LiveSender` / `SendQueue` and passed up from the transport's reactor
entry points, never from inside `send()`. Tested from `LiveSender` and `SendQueue` up to two
nodes over either transport.

**L12 — Live outcomes and ranks in the C ABI** (`rats_send` with a tag and a rank, a
`rats_on_live_outcome`), for the bindings; nothing uses them yet.

**L4 — Priorities inside `Priority::Live`.** ✅ *Needed by 2.1.* Audio > keyframe / base layer >
deltas > enhancement layers. `SendOptions::rank` (0–3): a lower rank's live messages leave ahead
of a higher one's whatever their deadlines, nearest deadline first within a rank. On UDP
`LiveSender` keeps a deadline-ordered queue per rank (expiry takes the earliest front of any, and
shedding under overload counts what leaves ahead in that order); on TCP `SendQueue` has a live
lane per (type, rank), each rank a scheduling class between Interactive and Normal. ratcast:
sound 0, keyframes 1, other frames 2 (`LiveRank`). Before it, EDF alone sent sound sent after a
keyframe behind the whole keyframe whenever the sound's deadline was the later — the same
lifetime, or longer as `cinema` will give it.

**L5 — Absolute deadlines when forwarding,** so latency does not accumulate per hop. *Needed by
stage 5's tree.*

**L8 — `send_many` with a shared refcounted buffer** for fan-out. *Improves stage 5's SFU.*

**L11 — One copy fewer on the send path:** `Node::send` copies the payload (`to_bytes()`) before
`LiveSender` copies it again. Measured in 0.6: the reactor thread uses 0.8% of a core at
8 Mbit/s, so it matters only for fan-out (with L8).

**L6 — FEC across messages** for small Opus packets — or rely on Opus in-band FEC + RED (2.1
decides).

**L9 — Path MTU discovery** above the fixed 1220-byte packets.

**L10 — A clock-sync service** (stage 5's synchronized playout; libratcast can do it over librats
ping instead).

**L7 — A delay-sensitive realtime mode** for BBR (or a GCC-style controller). *Optional.* What
it was for — a window that holds a keyframe on a path whose round trips swing by several times
their minimum (netem's `reorder`) — came with L2 (c): `reorder` plays at 59 fps. Taken up again
only if a measured path, or calls (stage 2), ask for lower queueing delay than BBR keeps.
