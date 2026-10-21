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
| Qt ≥ 6.6 with Quick, Shader Tools, QtGui private headers (QRhi) | app | system | official installer / aqtinstall |
| libopus ≥ 1.3 | audio | system (`libopus-dev`) | vcpkg |
| WebRTC AudioProcessing 1.x | voice: echo, noise, gain | system (`libwebrtc-audio-processing-dev`) | build from freedesktop.org's meson project |
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
Stages keep their numbers when their order changes (steps and `docs/bench.md` refer to them):
the app (6) comes before watching together (5), as one viewer is enough for its first version.
Every step ends with tests (and benchmarks when it moves a number) and leaves the code in its
final place and shape: stage 0 is a walking skeleton, not a throwaway prototype.

| Stage | Result | librats needs |
|---|---|---|
| **0. Skeleton** ✅ | screen → encoder → libratcast → librats → decoder → window, on Linux; latency and behaviour on emulated networks measured | L1 ✅ |
| **1. A stream that holds** ✅ | one video stream to a few viewers plays on every emulated path: rate control, fast joining, smooth playback on lossy paths | L2 (c) ✅, L3 ✅; L2 (a, b) put off |
| **2. Sound** ✅ | audio, A/V sync; latency profiles put off to stages 5 and 6 | L4 ✅ |
| **3. Standalone library** ✅ | MPEG-TS in and out, `rats://` URLs, a benchmark against librist and SRT, C API | — |
| **4. Platforms** — put off | GPU paths and hardware codecs; Windows and macOS capture; netsim ✅ | — |
| **5. Watch together** — after 6 | one-to-many: forwarding tree, simulcast / SVC, SFU, SFrame E2EE, synchronized playout | L5 |
| **6. App** ← now | Qt app MVP, one to one: invites, voice with AEC / NS, screen / window / app-audio share, camera; on Linux first, the other OSes as stage 4 reaches them | — |
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

### Stage 2 — Sound ✅

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

### Stage 3 — Standalone library ✅

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
  - (b) ✅ the tools: `ratcast-send --input` and `ratcast-recv --output` for TS over UDP
    (multicast joined), files (a recording played in real time by its PCR) and pipes;
    `ratcast-recv rats://host:port` or `rats://<peer id>`. The engine reads and writes the
    feeds (`open_ts_input()` / `open_ts_output()`, POSIX for now); the client's
    `DataPublisher` finds the 188-byte packets and sends six to a frame — a librats datagram
    — and `DataPlayer` writes them out through a `DeliveryBuffer`, what it holds at the
    feed's end included. A feed piped through on one host comes out byte for byte.
- **3.2 Benchmark** ✅ against librist and SRT (libwebrtc if practical). librats: L9 optional.
  - (a) ✅ the harness: `ratcast-tsprobe` — a stamped constant-bitrate TS probe (`send`) and its
    meter (`meter`: latency, loss after repair, continuity-counter jumps, the longest gap; the
    client's `TsProbeWriter` / `TsProbeMeter`); `bench/peers.sh`, SRT 1.5.7's
    `srt-live-transmit` and librist 0.2.20's `ristsender` / `ristreceiver` built from pinned
    commits without root; `bench/netem.py --feed`, each of the three over the same scenarios,
    with what crossed the link each way and each end's CPU. A first look (one run, 8 s): on
    `loss-2` ratcast loses 0.9% where SRT and librist lose nothing — all of it in the first
    second after joining, frames that expired in the sender's queue (`live_expired`), none
    after; it sends more back (0.35 Mbit/s against 0.05) and takes 2–3× their CPU. The
    joining loss is the round trip, not the loss (`rtt-50` 1.4%, `rtt-100` 6.8%, `rtt-20`
    none): librats' `LiveSender::shed` judges what cannot leave in time by BBR Startup's
    pacing rate, a fraction of what it is a few round trips on, and drops frames of a
    full-rate feed that would have made it (`docs/bench.md`, "Why ratcast loses on joining");
  - (b) what the first look asks for (`docs/bench.md`, "Why ratcast loses on joining" and "Where
    the rest goes"), each checked with a small run, then the comparison. In order:
    1. ✅ librats L2 (b): until the controller has a verdict on the path it sends no slower than
       live data is offered — the feed's own rate, measured by the connection — so shedding by
       the pacing rate no longer judges a full-rate feed by Startup's first few packets a round
       trip. Joining now loses nothing: `rtt-50` 1.4% → 0, `rtt-100` 6.8% → 0, `loss-2` 0.9% → 0
       (`docs/bench.md`, "Joining, fixed");
    2. ✅ `DataPublisher` sends full frames, six packets each; what is left of a read waits for
       the next, or until the input has been quiet for `hold` (5 ms; `TsInput::start` takes the
       quiet it reports, so no timer of the publisher's own), the input's end or stop(). A
       frame's capture time is its first packet's read, so what waited plays out on time.
       1520 → 886 packets a second at 8 Mbit/s, the link's overhead +7–10% → +3–7%, the
       feedback 0.37 → 0.21 Mbit/s. A feed slower than ~2 Mbit/s, read more than 5 ms apart,
       still goes as a full frame and a short one;
    3. ✅ `DataPlayer` is woken only by a frame due sooner than the one it waits for, and writes
       the frames due together at once: the viewer's `futex` 4000 → 1300 a second, `sendto`
       1500 → 865, its system calls ~9500 → ~5650 (srt-live-transmit ~3900). CPU, one host:
       7.5 / 8.1% → 6.2 / 6.2% against SRT's 2.8 / 3.1% — the rest per reactor turn in
       librats (L15) and a timer wake-up per frame at the viewer's writer (`docs/bench.md`,
       "Full frames, the writer woken less");
    4. librats L13, tried and put off: the ACKs are what librats' BBR reads the queue off, and
       fewer of them cost live video more than the 0.22 Mbit/s back they save (`docs/bench.md`,
       "Fewer ACKs, tried"); doing it right takes receive timestamps;
    5. ✅ the full table: `bench/netem.py --feed --repeat 3`, every scenario (`docs/bench.md`,
       "Against SRT and librist, measured whole"). Nothing lost where the path has room, by any
       of the three; the same latency, ratcast's p99 the closest to its median (4–8 ms against
       8–16); over capacity librist loses least, then ratcast, then SRT, which alone holds its
       latency. Where ratcast is behind: joining (~2 round trips more than librist), CPU (twice
       theirs: L15), feedback (0.2 Mbit/s against 0.02–0.05: L13), and `reorder` — every packet
       on its own delay — 0.35% lost to BBR's ProbeRTT, whose window is half a BDP at an outlier
       minimum round trip (L16).

    Left as they are: joining ~100 ms slower than librist (the handshake, then the stream's
    announcement and subscription, before the first frame: L2 (a) and 1.2's leftovers), and
    librats' portable cipher (L14).
- **3.3 C API** ✅ (`ratcast/ratcast.h`, in libratcast): media streams over a librats node made
  in C. librats' C ABI makes the node and connects it — discovery, NAT traversal, relays —
  and `ratcast_attach()` adds ratcast to it before `rats_start()`, through librats'
  `rats_attach_subsystem()` (`bindings/rats_extend.h`, C++ only: the door for libraries that
  layer a C API of their own on librats'). A sender offers streams — a description, bitrate
  limits if rate-controlled — and sends their encoded frames (copied once); a receiver
  subscribes to what peers announce and gets frames at a handler, with the announcements,
  withdrawals, keyframe and bitrate requests as handlers too, and the stats. A stream
  subscribed with a delay comes out in order and evenly on a thread of its own, as SRT and
  librist hand on a feed: a `DeliveryThread` (`stream/`: a DeliveryBuffer played out, which
  the client's `DataPlayer` now runs on too), its withdrawal told once what it held has gone
  out; a handler may unsubscribe its own stream. Zero-initialisable structs, every enum
  int-based, nothing unwinds across it. Tested on two real nodes from C++ (`test_capi.cpp`) and
  from C (`test_capi_c.c`, built as C11). Not in it: a feed's packet framing (`DataPublisher`'s
  six packets to a frame — a C sender sends whole packets, `RATCAST_DATA_FRAME_SIZE` bytes at
  most a frame) and the engine's capture and codecs, which bindings of the app will want
  (Android, stage 8).

netsim, the userspace impairment proxy (delay, jitter, loss, bursts, bandwidth caps; no root, any
OS; scripted scenarios in CI), moved to stage 4: it is for the other platforms, and
`bench/netem.py` covers Linux.

Closed with the full table, every scenario measured three times against SRT and librist (3.2 (b)
5; `docs/bench.md`, "Against SRT and librist, measured whole"): nothing lost where the path has
room, the same latency with ratcast's p99 the closest to its median, and over capacity between
librist and SRT. 3.3 moved nothing on the wire; what it touched of the feed's path, `DataPlayer`
now on a `DeliveryThread`, was checked with a small run (`local`, `loss-2` three times: as in
the table). Left where ratcast is behind, each with its librats item: joining ~2 round trips
slower than librist (L2 (a), 1.2's leftovers), CPU twice theirs (L15), feedback 0.2 Mbit/s
against 0.02–0.05 (L13), and `reorder`'s 0.35% lost to ProbeRTT (L16).

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
- **4.5 netsim** ✅ (from stage 3): `ratcast-netsim`, a UDP proxy whose two directions are
  emulated links following netem's rules — loss at random or in Gilbert-Elliott bursts, a queue
  of `limit` packets, a delay with each packet's own jitter or, with a `rate`, in order at it,
  `reorder` — in netem's own words, so `bench/netem.py --netsim` runs every scenario through it
  on loopback: no namespaces, no tc, no root, any OS (librats' sockets and poller). The link is
  tested in virtual time against netem's shares and orders, the proxy on loopback, and ratcast
  repairing 5% loss across it between two real nodes (`tools/tests`, every OS in CI); CI's
  Linux release job runs three scenarios through it end to end. Against netem (`docs/bench.md`,
  "netsim, against netem") the same paths to a few milliseconds, but under a cap: netem on the
  sender's own veth takes librats' GSO runs for one packet each, which adds ~30 ms to ratcast's
  p99 on `cap-drop`; netsim, which sees packets as a router does, does not. Its timers are the
  host's: on this VM up to 10 ms late.

Put off after 4.5: 4.1–4.4 need machines the dev VM is not — an Intel or AMD GPU with VA-API,
NVIDIA, Windows, a Mac — and are taken up a step at a time as one is at hand. The app (stage 6)
goes ahead meanwhile on Linux with the software codecs, which carry a one-to-one call and a
1080p screen; high resolutions and the other OSes come to it with these steps.

### Stage 5 — Watch together

The main use case: one person streams, many watch. After the app (stage 6), whose first
version is one to one, over the path straight between the two. Coarse until reached.

- Capacity-aware forwarding tree: viewers with spare uplink re-forward without transcoding.
  librats: **needs** L5.
- Simulcast / SVC layer selection per receiver.
- SFU mode and the `server` node for big rooms and hard NATs. librats: **better with** L8, L11.
- SFrame E2EE, room key from the owner over Noise (MLS later).
- Synchronized playout: clock sync, a common playout delay. librats: L10 optional.
- The `cinema` latency profile (from 2.3): seconds of buffer for retransmission to repair
  anything, held as encoded frames (decoded 1080p is ~3 MB a picture), lip sync over it.

### Stage 6 — App

The Qt 6 / QML app, one to one: one shares a screen and talks, the other watches and talks back,
over the path straight between them. Linux first, on the software codecs; Windows and macOS as
stage 4 brings them. *Done when* two people on two hosts call each other from an invite link,
one shares a screen with its sound, both talk with echo cancelled, and the tools and the app run
on the same call code.

- **6.1 A call in `client`** ✅: `Call`, what `ratcast-send` and `ratcast-recv` wired together by
  hand, as one object the app and the tools share — a librats node with its identity kept in a
  data directory; invites (`rats://PEER-ID` when discoverable through the DHT, `rats://ADDRESS:PORT`
  for each of the host's addresses, loopback and link-local left out) and dialing one; publishing
  sources it owns, each publisher hearing the keyframe requests and bitrates of its stream; playing
  the first video, sound and feed the other side offers, in step, and the next of a kind when one
  ends; events for a UI (joined, left, playing or failed to, ended, a source ran out), never under
  its locks, none once it is being destroyed. `rats_url.h` reads and writes the links. Both tools
  are a `Call` and a printer now, their output and summaries unchanged (a small netsim run of
  video, sound and the feed: as in `docs/bench.md`); `ratcast-send` prints the real invites. Tested
  between real nodes on loopback: both sides sending pictures and sound and playing the other's,
  hanging up ending what the other played, a stream withdrawn and offered again, a call that plays
  nothing never subscribing, a source running out, ids in use, invites; TSan and ASan clean.
- **6.2 Pictures in QML** ✅: `VideoView` (`app/video`, QML module `Ratcast.Video`), a Qt Quick
  item that draws the newest picture a player hands its `sink()` from any thread, the older ones
  skipped. The planes go up as they are — R8 for Y, U and V, RG8 for NV12's chroma, rows read in
  place at their stride — into textures made through QRhi when the size changes, and a shader
  (Qt Shader Tools at build time) turns them into colour with `rgb_from_yuv()`, the engine's
  inverse of its conversion (BT.709 / BT.601, limited / full); no Qt Multimedia. Fitted, its shape
  kept, centred; the newest picture is kept so that a scene graph made anew (the window hidden and
  shown, the GPU lost) draws it again. The sound needs nothing from Qt: `AudioOutput` plays it.
  Tested drawn for real, read back from windows never shown: both layouts in the four colour
  spaces, an odd size, a picture of another shape and a resized view refitted, clearing; the
  matrix against the conversion on the CPU. CI draws them on Mesa behind Xvfb; ASan clean (Mesa's
  and fontconfig's lifelong allocations suppressed). Qt 6.6 is the floor now, for QRhi.
- **6.3 The first app** ✅: share a screen or a window with the computer's sound, or join from a
  `rats://` link (typed, or on the command line: `ratcast rats://…`); the sharer sees who is
  watching and the links to send, with a copy button; the viewer sees the picture, hears the
  sound, and is told when the sharer goes. `client/screen_share.h` opens a share — the desktop's
  dialog, its choice kept in the data directory — and publishes it (pictures rate-controlled up
  to 8 Mbit/s, the sound as music), which `ratcast-send` now shares too. `app/call`'s
  `CallController` keeps the state for QML (Idle → Choosing → Sharing; Idle → Joining ⇄
  Watching), runs the dialog off the GUI thread and drops what an earlier call or share says
  late; the sharer's identity is kept (its peer-id link holds across runs) and a viewer has one
  of its own. Tested between two controllers on loopback with the pattern and a tone standing in
  for the desktop: share, join, watch, leave either side; a share that runs out, is declined,
  fails, or is called off while the dialog is open; a link that is none. Tried live: the app
  watching `ratcast-send`'s pattern, rate control climbing as with `ratcast-recv`.
- **6.4 Voice both ways** ✅: both sides talk. The engine's `AudioProcessor` is WebRTC's
  AudioProcessing (BSD, `RATCAST_WITH_WEBRTC_APM`): AEC3 against what the speaker plays, noise
  suppression, AGC2, voice detection; blocks of any length in, 10 ms out with their capture times,
  the echo's delay from play and capture times (tested in virtual time: 36 dB of a speaker's echo
  taken out, 18 dB of steady hiss, a vowel told from silence). `AudioMixer` lets several players
  share one output, so a call plays the sharer's voice — a sound stream labelled `voice`, in its
  own time — beside the screen's sound, which stays in step with the pictures. Client's
  `VoiceChannel` puts the processor between the microphone and the speaker it wraps; muted, it
  sends silence, which DTX makes next to nothing, so unmuting is at once; it says who is speaking
  and who is heard, held 300 ms. The app: a microphone and a voice for each side, a mute button
  that holds from one call to the next, a dot for each side speaking; no microphone is said and
  the call goes on. Tested through calls on loopback (each hears the other, a muted side is not
  heard) and through two controllers; ASan / TSan clean. Left for 6.6, and done there: a sharer's
  whole-system sound also carried the viewers' voices as the sharer's speaker played them — they
  heard themselves, late — and the sharer's microphone heard the screen's sound with nothing to
  cancel it against.
- **6.5 Camera, and two streams on one path:** a camera beside the screen; each stream took the
  whole path (1.1), so the path's rate is shared between them now.
  - **(a) The path shared, the camera in engine** ✅: the rate-controlled streams a peer watches
    share its path — each told of its part of what the path carries and delivered (`share_of()`),
    in proportion to the most it may take, which leaves nothing over for a stream at its most
    while the others want more; an overused path cuts them all, together to 85% of what it
    delivered (tested in virtual time: a screen of 8 Mbit/s and a camera of 2, 1.36 and
    0.34 Mbit/s from 2 delivered; a stream the peer does not watch takes no part).
    `list_cameras()` / `open_camera()`: PipeWire's video sources of a camera's role (V4L2 and
    libcamera devices both), captured as a screen is, in the formats cameras send — YUY2 (a new
    read-only `PixelFormat`, into 4:2:0 by `convert_frame()`: 46 µs at 720p), NV12, I420, BGRx, a
    block for all planes or one each — in their own colour space (BT.601 unless they say); the
    first mode no larger than asked at 15 pictures a second or more, else no larger, else any.
    MJPEG modes are not taken yet (a USB 2 camera's 720p at 30 is often only MJPEG). Tested
    against a stand-in camera node: listed, a screen not; each format and layout back into 4:2:0
    exactly; one with only larger modes taken as it is; the end when it goes; ASan / TSan clean.
    `ratcast-send --source camera`. No camera on the dev VM: not tried on a real one yet.
  - **(b) The camera in a call** ✅: either side may send its camera beside the rest — a video
    stream labelled `camera` (`kCameraLabel`), small (640 x 480, 30 a second, 1 Mbit/s at most,
    a ninth of a path shared with an 8 Mbit/s screen) — and a call plays the other side's camera
    apart from its screen, in its own time, as a voice is apart from the screen's sound;
    `with_preview()` shows the sender what goes. The app: a camera button that holds from one call
    to the next and turns itself off (said) where there is no camera; the other side's camera in
    a corner over the picture, one's own below it as a mirror shows it; a camera going ends the
    camera, not the share. `ratcast-send --source camera` sends it as the picture itself.
    Found on the way: a picture starting to play wiped any notice, so "no microphone" and "no
    camera" were gone a moment after they were said; only "the other side left" goes now.
    Tested through calls on loopback (each picture where it belongs, at its size; a camera
    turned off, the screen going on; a camera nobody shows, never received) and two controllers
    (each side's camera at both ends, off again, kept across calls; none there: off, said, the
    call going on); ASan / TSan clean. The C API's test withdrew its feed before the last frames
    were in, which a loaded machine could see overtaken; it waits for them now.
- **6.6 App audio** ✅: a share's sound is what the applications play, never the call's own
  voices: `open_audio_capture()`'s `Applications` source, on Linux a PipeWire filter of its own
  with a left and a right input, which links itself to the output ports of every node playing
  sound ("Stream/Output/Audio") whose client is not this process (its `application.process.id`,
  so a sandbox would not fool it; the daemon's pid where it says none) — left to left, right to
  right, centres and mono to both, the LFE to neither, PipeWire adding up what comes in; mono is
  the two sides' mean. Clients, nodes and ports are followed in the registry, so a player started
  later is heard as it starts; the filter is processed with nothing linked, so the sound runs on,
  silent. It has no converter: a graph at another rate than 48 kHz ends it (Unsupported; the
  graph is asked for 48 kHz). The echo: `VoiceChannel` takes the system's sound (the default
  output's monitor) as what can come back in place of what its speaker plays, while the
  microphone runs — the film the sharer's microphone hears is cancelled, the viewers' voices
  still are, and so is any other application on either side. `ratcast-send --audio apps` shares
  the same. Tested on the user's daemon, each test at a pitch of its own, measured there
  (Goertzel), as tests run side by side: this process's tone heard in both channels when another
  is left out, not heard when it is, the sound going on without it, a player starting later heard
  at the two sides' mean; the system's sound as the echo in virtual time (a film the speaker never
  played: 84% of it comes back without, < 1% with); ASan / TSan clean. Found on the way: the
  daemon's word that an object just went (a port before its link was made, a client as it was
  bound) came as an error on the core, which ended the capture; only the connection lost does
  now. Later: a choice of one application (a picker; the desktop's dialog does not say whose a
  window is); linking needs the daemon's permission to make links, which a Flatpak sandbox will
  have to be given.
- **6.7 Latency profiles** (from 2.3) — put off again: nothing in a one-to-one call asks for a
  choice yet, every knob a profile would set already follows the path, and the one setting there
  is (200 ms) serves both a film and a talk. Back when a use wants another — a game's pictures
  not waiting for its sound, `cinema`'s seconds of buffer in stage 5 — as `call` / `game` for the
  user to choose: lifetimes, jitter-buffer depth, keyframe policy, encoder settings, the sound's
  frame for FEC only on lossy paths.

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
| | L2 (b) | A live flow's start: shedding by Startup's rate | needed | 3.2 (feeds); 1.2 (long paths) | ✅ |
| 1 | L2 (a) | A live flow's start: the initial window | needed | 1.2 (long paths) | put off; eased by (b) |
| 2 | L5 | Absolute deadlines when forwarding | needed | stage 5 (tree) | |
| 3 | L15 | Fewer system calls per reactor turn | improves | 3.2 (CPU) | |
| 4 | L16 | ProbeRTT that does not starve a live flow | improves | 3.2 (`reorder`) | |
| 5 | L8 | `send_many` with a shared buffer | improves | stage 5 (SFU) | |
| 6 | L11 | One copy fewer on the send path | improves | with L8 | |
| 7 | L6 | FEC across messages | optional | 2.1 | if Opus FEC + RED fall short |
| 8 | L9 | Path MTU discovery | optional | 3.4, 4K | |
| 9 | L10 | A clock-sync service | optional | stage 5 (playout) | libratcast can do it over ping |
| 10 | L7 | A delay-sensitive realtime mode | optional | — | mostly done with L2 (c) |
| 11 | L12 | Live outcomes and ranks in the C ABI | optional | other bindings | when one needs them |
| 12 | L13 | Fewer ACKs for live data | optional | feedback on a narrow uplink | tried: needs receive timestamps |
| 13 | L14 | A faster ChaCha20-Poly1305 | optional | — | ~0.5% of a core at 8 Mbit/s |

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
- (b) *Needed by 3.2, and by 1.2 on long paths.* Shedding by the pacing rate drops the frames
  behind a keyframe while that rate is still Startup's floor (~0.5 Mbit/s at 200 ms RTT): with
  the joining keyframe let arrive late (1.2), up to 8 frames behind it are shed, and the keyframe
  that has to follow is a 0.4 s stall after the first picture. A TS feed, at its full rate from
  the first packet, meets it from a 50 ms round trip: `LiveSender::shed` gives up a message when
  `now + one_way + rate.time_for(ahead + owed) > deadline` by Startup's rate (2.2 Mbit/s at
  50 ms, 1.1 at 100 ms, doubling each round), and 99 frames go at 50 ms, 456 at 100 ms — all
  of them before the earliest deadline, and the backlog would have drained in time
  (`docs/bench.md`, "Why ratcast loses on joining"). ✅ Not shedding in Startup was no way out:
  in virtual time it saved the 50 ms path and lost more on longer ones (100 ms 6.4% → 7.9%,
  200 ms 20% → 54%) — a feed faster than Startup's climb builds a backlog that shedding is what
  clears. What does is a start at the feed's own rate: until the controller has a verdict on
  the path (BBR's Startup while it has measured less than the feed, Reno's first slow start)
  its pacing rate is no lower than what live data is offered at — `udp::OfferedRate`, over the
  last second, sampled by the connection and passed in `CongestionController::on_live_offered`
  — and its window lets that through (`cc::live_floor_window()`, twice what the rate holds over
  the round trip). The floor is the feed's own load, never more; a quarter over drained the
  first round trip's backlog sooner but made a third more frames late with seven connections
  starting on one uplink. The shedding itself is unchanged. Virtual time, a feed of 8 Mbit/s
  with 2% loss: 50 ms 1.5% → 0, 100 ms 6.4% → 0.1%, 200 ms 20% → 3% (its lifetime of 200 ms
  leaves no time to repair). `bench_path --realtime` over 8 seeds: better or level nearly
  everywhere — fixed-rate video on the far path 2.6–6.7% of frames late → 0.5–2.3%, 200 ms with
  1% loss 3.7% → 1.0%, the bulk-flow rows 22% → 16% and 30% → 25%, seven peers joining 3 s apart
  4.8% → 3.4%; worse by a few tenths: seven peers starting together on a 20 Mbit uplink (3.3% →
  3.6%) and some columns of video near a 1–2 Mbit link's rate. It also brings the joining
  keyframe sooner (L2 (a)): ratcast's video at `rtt-100` shows its first picture in 364 ms where
  stage 2 took 638.

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

**L13 — Fewer ACKs for live data.** *Optional; tried, put off.* An ACK every second packet
(RFC 9000) is ~440 a second for an 8 Mbit/s feed in full frames — 0.22 Mbit/s back (2.7% of the
feed) where SRT sends 0.05 (an ACK every 10 ms). Tried as QUIC's ACK_FREQUENCY (a frame, wire
v7): the sender asks for every max(2, window / 4) packets of live data, the receiver still
acknowledging at once a packet out of order and the third past a gap, and BBR told how many
come together so it does not read them as a path that holds acknowledgements back. It does not
pay, because those acknowledgements are BBR's sensor: it reads the queue off them ack by ack
(Startup ends on six groups of them rising in a row; a probe on one lifted past its bar), with
allowances tuned to an ACK every second packet (`kAckedTogether`). Allowed in Startup, video's
Startup never ended; kept out of it, a constant-rate feed on a path it never fills — the TS feed
on every netem path but the capped ones — stays in Startup for good and gains nothing; between
probes, even with at most four packets to an ACK, `bench_path` lost ground where live video
needs the controller most: beside a bulk flow 12.2 → 20.5% of frames late, a link dropping to
3 Mbit/s froze for 2.3 s instead of 0.6. Fewer ACKs without a blinder controller means
receive timestamps (WebRTC's transport-wide feedback, QUIC's receive-timestamp drafts): the
receiver says when each packet arrived, in batches, and the controller reads the queue off that
instead of off when acknowledgements come back — a rework of BBR's queue signals, for when the
viewers' uplinks ask for it. The attempt, should it be wanted: `docs/bench.md`, "Fewer ACKs,
tried".

**L15 — Fewer system calls per reactor turn.** *Improves 3.2.* At 886 packets a second the
reactor makes system calls of its own each turn beyond the data's: it empties its wake-up socket
with a `recv` every turn whether or not the poller reported it (all ~830 a second fail at the
viewer), follows every `recvmmsg` that found datagrams with one that finds none, and `post()`
sends a wake-up byte for every task though one may already be pending (~650 a second at a
sender, one per frame). Drain only when the poller reported the socket (a byte written after
the wait is still there for the next one), stop reading when a batch comes back short, and
signal only when no wake-up is pending.

**L16 — ProbeRTT that does not starve a live flow.** *Improves 3.2.* On a path that delays every
packet on its own (netem's `reorder`: N(25 ms, 10 ms) each way) BBR's minimum round trip is an
outlier — 3 ms where the round trip is ~50 — so an app-limited sender never sees it again
within an eighth, never skips ProbeRTT, and enters it every 5–8 s with a window of half a BDP
at that minimum: 6.4 KB where an 8 Mbit/s feed keeps ~50 KB in flight. 32–72 frames are shed an
episode (in virtual time, three seeds; on netem one burst of 36–45 in each 15 s run, 0.35% of
the feed), once followed by the estimate falling to 1.1 Mbit/s. Either a ProbeRTT window no
smaller than the live floor (`cc::live_floor_window()`, as Startup's since L2 (b)), or the skip
taken while samples stay within the path's own swing of the minimum. `docs/bench.md`, "Why
ratcast loses on `reorder`: ProbeRTT".

**L14 — A faster ChaCha20-Poly1305.** *Optional.* The vendored portable code costs ~15
instructions a byte, a third of librats' user time — still ~0.5% of a core at 8 Mbit/s, so
nothing asks for it yet; a vectorised one when 4K or many viewers per sender do.

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
