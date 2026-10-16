# ratcast — measurements

What the pipeline costs and how it behaves on a network, measured. Every number here says what
it was measured on and with which command, so it can be measured again after a change.

## How to measure

**Micro-benchmarks** (`bench/`, on librats' harness), from a release build:

```bash
cmake --preset release -DRATCAST_BUILD_BENCH=ON && cmake --build --preset release
./build/release/bin/bench_core      # Buffer, frame header
./build/release/bin/bench_session   # MediaSession per frame
./build/release/bin/bench_codec     # encoders and decoders
./build/release/bin/bench_convert   # BGRA -> NV12 / I420, against libswscale
```

**The whole pipeline over an emulated network** (`bench/netem.py`): `ratcast-send --source
pattern` in one network namespace, `ratcast-recv --headless` in another, the veth pair between
them shaped in each direction by `tc netem`. No root: the namespaces belong to a user namespace
of the script's own. Each run is a fresh sender and viewer, so joining is measured every time.

```bash
bench/netem.py --list                       # the scenarios
bench/netem.py --repeat 3                   # all of them; prints a Markdown table
bench/netem.py rtt-100 loss-2 --size 1280x720 --bitrate 4000
```

The test pattern carries its capture time (the time of day) in every picture, so the viewer reads
the latency from capture to decoded picture off each one. Both tools end with a `summary` line
of `key=value` pairs; the table is made from those.

**Two hosts:** `ratcast-send --source pattern` on one, `ratcast-recv --connect HOST:7700` on the
other. With the clocks synchronised (chrony / NTP on a LAN is within a millisecond) the viewer
prints the latency as on one host; otherwise it prints none.

**In a window:** `ratcast-recv` without `--headless` also prints `shown`, the latency to the
picture presented (with vsync, where the driver has it) — capture to screen, but for the
compositor's own frame.

## The machine

The dev VM all numbers below come from: VMware guest on an Intel Core Ultra 7 265KF, 16 vCPUs,
Linux 7.0, FFmpeg 8 (libavcodec 62.11), SVT-AV1 2.3.0, dav1d 1.5.3, no GPU codecs (`vmwgfx` has no
VA-API). Release build. One host: both tools share the CPU, which flatters nothing — they are far
from saturating 16 cores.

## Baseline, 0.6

1080p60 test pattern, AV1 (SVT-AV1, preset from `video_encoder.cpp`), 8 Mbit/s, frame lifetime
200 ms, joined one second into the stream.

### On one host

| | p50 | p95 | p99 |
|---|---:|---:|---:|
| capture → decoded picture | 17.9 ms | 30.2 ms | 34.8 ms |
| capture → presented (SDL window, this VM's display) | 21.0 ms | 34.8 ms | 38.9 ms |

The 18 ms are drawing the pattern (~6 ms: its stamp is taken before it is drawn), encoding (~9 ms,
SVT-AV1 at 1080p) and decoding (~3 ms, dav1d); hand-overs between threads and the network add
next to nothing at the median. The window adds ~3 ms (texture upload, present).

CPU, per thread from `/proc` over 8 s at 1080p60, 8 Mbit/s (100% = one core):

| sender | | receiver | |
|---|---:|---|---:|
| SVT-AV1 (≈60 threads) | ~100% | dav1d | 24% |
| test pattern, drawing 1080p | 36% | everything else (network, player) | 2% |
| publisher thread (session, sending) | 3.8% | | |
| librats reactor | 0.8% | | |

The test pattern's drawing is the test source's own cost; screen capture converts a picture in
~1 ms (`bench_convert`). librats moves 8 Mbit/s for under 1% of a core, so the second copy on its
send path is not worth removing before fan-out needs it.

### Across an emulated network

`bench/netem.py --repeat 3 --duration 15`. Delays are one way, so a scenario's round trip is
twice its delay; loss is on the media's direction unless said otherwise. *First picture* counts
from dialling (handshake, announce, subscribe, keyframe). *Stalls*: gaps between pictures over
100 ms, how many and how long in all, in 15 s. *Dropped*: frames that arrived but could not be
decoded (a reference lost). *Expired / sent*: live messages the sender gave up on.

| scenario | path | first picture, ms | fps | latency p50 / p95 / p99, ms | stalls (ms) | lost / dropped | expired / sent | keyframes | repair | CPU send / recv, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| local | veth, no shaping | 52 | 60 | 16.9 / 29.7 / 32.8 | 1 (221) | 0 / 0 | 0 / 888 | 2 | 0.0% | 140 / 25 |
| rtt-20 | 20 ms RTT | 206 | 60 | 30.2 / 45.1 / 73.7 | 0 (0) | 0 / 0 | 0 / 828 | 2 | 0.0% | 143 / 25 |
| rtt-50 | 50 ms RTT | 487 | 60 | 47.1 / 64.5 / 69.6 | 0 (0) | 0 / 0 | 8 / 882 | 3 | 0.0% | 141 / 25 |
| rtt-100 | 100 ms RTT | 941 | 58 | 73.7 / 92.2 / 129.0 | 2 (656) | 1 / 29 | 14 / 808 | 5 | 0.0% | 144 / 24 |
| rtt-200 | 200 ms RTT | 1993 | 60 | 124.9 / 143.4 / 159.7 | 0 (0) | 0 / 25 | 40 / 862 | 8 | 0.0% | 144 / 22 |
| jitter | 50 ms RTT, ±10 ms jitter, in order | 594 | 51 | 86.0 / 151.6 / 196.6 | 2 (1005) | 11 / 87 | 30 / 883 | 8 | 0.0% | 147 / 21 |
| reorder-2 | 50 ms RTT, 2% of packets 25 ms early | 570 | 26 | 131.1 / 208.9 / 225.3 | 11 (7945) | 112 / 295 | 204 / 826 | 27 | 0.1% | 150 / 12 |
| reorder | 50 ms RTT, ±10 ms jitter, every packet on its own | -1 | 0 | 176.1 / 225.3 / 227.7 | 0 (0) | 0 / 427 | 530 / 886 | 55 | 0.0% | 153 / 1 |
| loss-1 | 50 ms RTT, 1% loss | 513 | 58 | 49.2 / 98.3 / 114.7 | 1 (718) | 1 / 14 | 9 / 826 | 4 | 3.0% | 145 / 24 |
| loss-2 | 50 ms RTT, 2% loss | 534 | 58 | 51.2 / 106.5 / 127.0 | 2 (493) | 2 / 4 | 9 / 860 | 4 | 5.3% | 144 / 25 |
| loss-5 | 50 ms RTT, 5% loss | 574 | 59 | 71.7 / 129.0 / 151.6 | 4 (452) | 0 / 0 | 8 / 869 | 3 | 10.1% | 146 / 25 |
| loss-both | 50 ms RTT, 2% loss both ways | 508 | 59 | 59.4 / 104.4 / 116.7 | 2 (238) | 0 / 0 | 8 / 873 | 3 | 5.5% | 145 / 25 |
| burst | 50 ms RTT, bursts of loss (~4%) | 539 | 60 | 47.1 / 108.5 / 139.3 | 4 (733) | 0 / 0 | 9 / 886 | 3 | 6.4% | 145 / 25 |
| cap-20 | 50 ms RTT, 20 Mbit/s, 100 ms buffer | 494 | 59 | 59.4 / 79.9 / 94.2 | 1 (171) | 0 / 0 | 8 / 875 | 3 | 0.0% | 146 / 25 |
| cap-6 | 50 ms RTT, 6 Mbit/s (below the stream) | 557 | 30 | 180.2 / 233.5 / 249.9 | 11 (7177) | 94 / 297 | 95 / 816 | 25 | 0.0% | 149 / 14 |

A stall or two on the clean paths (`local`, `rtt-20`) is the VM, not the network: the sender
itself drops to ~40 fps for that second, its whole process descheduled.

What it says:

- **The steady state is sound on clean and lossy paths.** Latency is the path's one-way delay
  plus the pipeline's ~18 ms at the median. Under 1–5% random loss, and in bursts, FEC and repair
  keep everything playing (repair costs 3–10% of the bitrate) at the price of a repair round trip
  in the tail: p95 grows from 65 to 100–130 ms at 50 ms RTT, and a lost reference shows as a
  stall of a few hundred ms until a keyframe — 1–4 in 15 s.
- **Joining is slow, and slower with distance.** 0.2 s at 20 ms RTT, 0.5 s at 50 ms, ~1 s at
  100 ms, 2 s at 200 ms, where it should be ~3 round trips plus the pipeline. See below.
- **Reordering breaks the stream** (fixed since, see below). With 2% of packets arriving 25 ms
  early (`reorder-2`) the sender reads 15–25% loss, sends the overtaken packets again and still
  lets frames expire: 26 fps and 8 s of stalls in 15. Per-packet jitter (`reorder`, everything
  reordered) plays nothing at all: 530 of 886 frames expire and 55 keyframes are asked for.
  RACK's adaptation to reordering (librats' `Recovery`) is off while live messages are
  outstanding — for a video stream, always.
- **Jitter without reordering costs more than it should**: +40 ms at the median over `rtt-50`
  and one run in three takes 1.8 s to join (BBR, below).
- **A path narrower than the stream collapses** (`cap-6`: 30 fps, half of every second
  stalled) — expected, there is no rate control yet (stage 1, item 1). With headroom
  (`cap-20`) the cap is invisible.

### Reordering, after librats item 1

The same runs after the fix (`PLAN.md`, "librats work", item 1): a path's learnt reordering is
waited out for live data too, unless the wait would leave a frame no time for a copy, and what
goes again sooner is a loss for BBR only once the reordering no longer explains it. Also a
packet number is no longer sent in one byte, which a packet overtaken by more than 127 others
(a keyframe's burst past one held back) did not survive.

| scenario | first picture, ms | fps | latency p50 / p95 / p99, ms | stalls (ms) | lost / dropped | expired / sent | keyframes |
|---|---:|---:|---:|---:|---:|---:|---:|
| reorder-2 (before) | 570 | 26 | 131.1 / 208.9 / 225.3 | 11 (7945) | 112 / 295 | 204 / 826 | 27 |
| reorder-2 | 536 | 60 | 71.7 / 86.0 / 124.9 | 0 (0) | 0 / 0 | 8 / 827 | 3 |
| reorder (before) | — | 0 | 176.1 / 225.3 / 227.7 | 0 (0) | 0 / 427 | 530 / 886 | 55 |
| reorder | 507 | 0 | 171.8 / 196.4 / 197.2 | 0 (0) | 294 / 518 | 451 / 883 | 53 |

The control scenarios (`local`, `rtt-50`, `loss-2`, `burst`, `cap-20`) are unchanged, and so is
a live run over a real path (this VM to a server ~40 ms away, `bench_transport --connect`).

- `reorder-2` plays clean. Its median is still ~24 ms above `rtt-50`'s: the early 2% give
  round trips of 25 ms, half the path's, and BBR's window (2 × bandwidth × minimum round trip)
  then holds a keyframe back at the sender. No real path delivers a packet faster than its base
  delay; netem's "early" packets do.
- `reorder` (each packet's one-way delay drawn on its own, normal 25 ± 10 ms: from ~0 to ~55 ms)
  still does not play. Its round trips range from a few ms to ~100 ms, several times the path's
  base, and two things give: the reordering window is capped at the smoothed round trip, which
  the newest acknowledgement — usually a packet that overtook — biases low; and BBR's window,
  built on a minimum round trip of a few ms, cannot hold a keyframe in flight. `bench_path`
  shows the edge: jitter up to the base round trip plays (50 ms + 0–40 ms each way: 1.7% late),
  several times it does not (10 ms + 0–40 ms: 97% late). A path like that is rare; it is BBR's
  problem more than the loss detector's, and stays on the list.

### Joining

Where the time goes at 20 ms RTT, from the tools' `--verbose` logs (one run):

| | ms |
|---|---:|
| handshake (Noise XX over UDP, one round trip) | 21 |
| the stream announced, the viewer subscribes | 46 |
| the subscription reaches the sender, which asks its encoder for a keyframe | ~56 |
| next picture, encoded as a keyframe (~52 KB at 8 Mbit/s) | ~85 |
| keyframe transferred, decoded: **first picture** | 207 |

Three quarters of it is the keyframe's transfer, and on longer paths it does not arrive at all.
A fresh connection paces at librats' initial window per round trip at Startup's gain:
4 × 1220 B × 2.77 per RTT — 676 KB/s at 20 ms, 270 KB/s at 50 ms, 135 KB/s at 100 ms. A 52 KB
keyframe takes 77, 190 and 385 ms to leave; with the one-way delay on top it is past its 200 ms
lifetime from 50 ms RTT up, and expires. The viewer asks again after `keyframe_retry`
(250 ms); the rate has grown by one step meanwhile, not by one per round trip, because a live flow
waiting for a keyframe sends nothing. At 100 ms RTT the fourth keyframe gets through, at 200 ms
the seventh.

The frames sent right behind the keyframe meet a second problem: `LiveSender::shed` drops a
message that cannot *leave* in time behind those ahead of it at the current pacing rate — and in
Startup that rate is the floor above, not the path. That is the expired frame the 0.5 integration
test saw under load.

A 10-packet initial window (RFC 9002's), measured by rebuilding librats with it:

| first picture, ms | 20 ms RTT | 50 ms | 100 ms | 200 ms |
|---|---:|---:|---:|---:|
| 4 packets (now) | 206 | 487 | 941 | 1993 |
| 10 packets | 177 | 336 | 897 | 1743 |

It gets the first keyframe through at 50 ms RTT, and not further. A smaller keyframe helps about
as much (at 2 Mbit/s, ~20 KB: 330 ms at 50 ms RTT, 700 ms at 100 ms). Beyond that, a joining
viewer needs either a keyframe allowed to arrive late — with per-message delivery feedback the
sender knows it is on its way and does not have to be asked again — or a rate control that starts
low and so makes the first keyframe small.

A viewer that subscribes within `min_keyframe_interval` (200 ms) of the last keyframe waits for
the interval to pass; the netem runs join one second into the stream so as not to measure that.

### What to do first

In librats (the list in `PLAN.md`, "librats work", is ordered by this):

1. ✅ Reordering must not read as loss while live messages are outstanding — the one failure here
   that stopped a stream outright, on paths (Wi-Fi, LTE, bonded links) that are common. Jitter
   several times the base round trip still breaks BBR (above).
2. The start of a live flow: a 10-packet initial window; no shedding by Startup's floor rate;
   BBR's app-limited Startup exit robust to jitter; a joining scenario in `bench_path`, which
   measures in virtual time what netem can only sample.
3. Per-live-message delivered / expired feedback, for keyframe recovery and joining.

In ratcast (stage 1): rate control first (`cap-6`), starting low so that a joining keyframe is
small; then joining with a keyframe allowed to arrive late.
