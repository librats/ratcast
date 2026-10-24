# ratcast — measurements

What the pipeline costs and how it behaves on a network, measured. Every number here says what
it was measured on and with which command, so it can be measured again after a change.

## How to measure

**Micro-benchmarks** (`bench/`, on librats' harness), from a release build:

```bash
cmake --preset release -DRATCAST_BUILD_BENCH=ON && cmake --build --preset release
./build/release/bin/bench_core      # Buffer, frame header
./build/release/bin/bench_session   # MediaSession per frame
./build/release/bin/bench_codec     # video encoders and decoders, Opus
./build/release/bin/bench_convert   # BGRA -> NV12 / I420, against libswscale; YUY2 -> NV12
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

**A room** (`bench/netem.py --room N`): N `ratcast-room` members, each in a namespace of its
own on one bridge, each talking (the stamped tone), with `--share` the first sharing the
pattern; each joins through the one before. Each member's uplink is the scenario's forward
direction, so a path between two members is one scenario long and a cap is what a member sends
to everyone together. "A room (10.5)" below.

**Without namespaces** (`bench/netem.py --netsim`, any OS): the same scenarios through
`ratcast-netsim`, a UDP proxy on loopback whose two directions follow netem's rules in user space;
the viewer dials the proxy, which forwards to the sender. How its paths compare with netem's:
"netsim, against netem" below.

**Two hosts:** `ratcast-send --source pattern` on one, `ratcast-recv --connect HOST:7700` on the
other. With the clocks synchronised (chrony / NTP on a LAN is within a millisecond) the viewer
prints the latency as on one host; otherwise it prints none.

**In a window:** `ratcast-recv` without `--headless` also prints `shown`, the latency to the
picture presented (with vsync, where the driver has it) — capture to screen, but for the
compositor's own frame.

## The machine

The dev VM all numbers below come from: VMware guest on an Intel Core Ultra 7 265KF, 16 vCPUs,
Linux 7.0, FFmpeg 8 (libavcodec 62.11), SVT-AV1 2.3.0, dav1d 1.5.3, libopus 1.6.1, no GPU codecs
(`vmwgfx` has no VA-API). Release build. One host: both tools share the CPU, which flatters nothing — they are far
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
~1 ms (`bench_convert`), a camera's YUY2 one in 46 µs at 720p (it is all copies and means). librats moves 8 Mbit/s for under 1% of a core, so the second copy on its
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
  stalled) — expected, there is no rate control yet (`PLAN.md`, 1.1). With headroom
  (`cap-20`) the cap is invisible.

### Reordering, after librats L1

The same runs after the fix (`PLAN.md`, librats track, L1): live data waits out a path's
learnt reordering as bulk does, the window may grow up to a probe timeout rather than a round
trip, and FEC counts that wait into the time a repair takes. Also a packet number is no longer
sent in one byte, which a packet overtaken by more than 127 others (a keyframe's burst past one
held back) did not survive.

| scenario | first picture, ms | fps | latency p50 / p95 / p99, ms | stalls (ms) | lost / dropped | expired / sent | keyframes |
|---|---:|---:|---:|---:|---:|---:|---:|
| reorder-2 (before) | 570 | 26 | 131.1 / 208.9 / 225.3 | 11 (7945) | 112 / 295 | 204 / 826 | 27 |
| reorder-2 | 564 | 60 | 70.7 / 86.0 / 112.7 | 0 (0) | 0 / 0 | 8 / 884 | 3 |
| reorder (before) | — | 0 | 176.1 / 225.3 / 227.7 | 0 (0) | 0 / 427 | 530 / 886 | 55 |
| reorder | 548 | 6 | 120.8 / 217.1 / 234.2 | 2 (5566) | 396 / 365 | 400 / 856 | 47 |

The control scenarios (`local`, `rtt-50`, `loss-2`, `cap-20`) are unchanged, and so is a live
run over a real path (this VM to a server ~40 ms away, `bench_transport --connect`: every frame
delivered, 41 ms median). `burst` stalls 8 times in 15 s at the median of ten runs against 7
with the previous librats on the same VM, the same repair share (9%): within its spread (3–14),
and `bench_path`'s rows with loss in bursts move both ways by seed.

- `reorder-2` plays clean. Its median is still ~24 ms above `rtt-50`'s: the early 2% give
  round trips of 25 ms, half the path's, and BBR's window (2 × bandwidth × minimum round trip)
  then holds a keyframe back at the sender. No real path delivers a packet faster than its base
  delay; netem's "early" packets do.
- `reorder` (each packet's one-way delay drawn on its own, normal 25 ± 10 ms: from ~0 to ~55 ms)
  still does not play: 6 fps. Its round trips range from a few ms to ~100 ms, several times the
  path's base. The reordering window now reaches that far, but BBR's window, built on a minimum
  round trip of a few ms, cannot hold a keyframe in flight. A path like that is rare; it is
  BBR's problem, not the loss detector's (`PLAN.md`, librats track, L7).
- `bench_path`'s reordering table (50 Mbit/s, 20 ms, four seeds) says the same in virtual time.
  With every datagram late by its own 0–10 ms, 1080p30, 1080p60 and 4K60 had 7–34%, 18–73% and
  90–99% of their frames late, and 12–53% of all packets went twice. Now every class has under
  1% of frames late and at most 0.1% of packets sent twice. The cost is 4K60, 40 Mbit/s on that
  50 Mbit/s link, where 0.1–5% of the datagrams are overtaken by 8 ms: 1–3% of frames late,
  where it had 0.3% (though at 5% only 0.4–0.5% of its packets go twice, not 5%). A real loss now
  waits out the learnt window too, and at 80% of the link there is little room to catch up
  after it. A rate control that leaves headroom (`PLAN.md`, 1.1) covers it.

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

In librats (`PLAN.md`, librats track, ordered by this):

1. ✅ L1: reordering must not read as loss while live messages are outstanding — the one
   failure here that stopped a stream outright, on paths (Wi-Fi, LTE, bonded links) that are
   common. Jitter several times the base round trip broke BBR still (above), until L2 (c).
2. L2, the start of a live flow: ✅ (c) BBR's Startup exit and probes robust to jitter
   ("Rate control (1.1)", below); left, (a) a 10-packet initial window and (b) no shedding by
   Startup's floor rate, with a joining scenario in `bench_path`, which measures in virtual time
   what netem can only sample.
3. L3, per-live-message delivered / expired feedback, for keyframe recovery and joining.

In ratcast (stage 1, "A stream that holds"): rate control first (1.1, `cap-6`), starting low so
that a joining keyframe is small (✅, below); then joining with a keyframe allowed to arrive late
(1.2).

## Rate control (1.1)

The session steers each stream's encoder by the paths to its viewers, sampled every 100 ms
(`RateController`, `ratcast/session/rate_controller.h`): it starts at 2 Mbit/s and ramps ×1.5 a
second; it cuts at once to 85% of what a full path carries when frames expire, a queue fills up
or a message waits in front of the path for a quarter of its lifetime; it climbs back carefully
towards where the path filled up. An encoder that cannot change its bitrate in flight (every
software one through FFmpeg) opens a new one alongside and switches to it with a keyframe.

Changing the bitrate, `bench_codec` (the new rows): the slowest of the next 30 pictures, taken at
the source's pace, and the keyframe the new encoder starts with.

| encoder | normal: mean / worst encode | bitrate halved: worst encode | keyframe |
|---|---:|---:|---:|
| SVT-AV1 720p, 4 Mbit/s | 5.3 / 12 ms | 14–26 ms | 12 KB |
| SVT-AV1 1080p, 8 Mbit/s | 7.8 / 18–22 ms | 25–31 ms | 24 KB |
| libaom 1080p, 8 Mbit/s | 19.6 / 26 ms | 61–65 ms | 11 KB |

Reopened on the encoding thread, as before, SVT-AV1 stopped for 55 ms at 1080p (39 ms at 720p):
three pictures at 60 fps, and a stall for a viewer at every change. Opening and closing are now
done alongside; libaom's first picture on a new encoder is slow in itself. SVT-AV1 keeps state
across encoders, so encoders open and close one at a time (ThreadSanitizer caught one opening
while another closed).

The netem runs, rate-controlled (`bench/netem.py --repeat 3`; `--send-arg=--fixed-bitrate` gives
the baseline above; `--bitrate` is now the top). The new column is what the viewer received over
the run, ramp included, and how many times the bitrate was cut. `cap-drop` is new: 20 Mbit/s
that falls to 4 for five seconds.

1920x1080 at 60 fps, AV1, at most 8 Mbit/s, frame lifetime 200 ms; 15 s a run, medians of 3.

| scenario | path | first picture, ms | fps | Mbit/s (cuts) | latency p50 / p95 / p99, ms | stalls (ms) | lost / dropped | expired / sent | keyframes | repair | CPU send / recv, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| local | veth, no shaping | 37 | 60 | 6.3 (1) | 16.4 / 30.2 / 38.9 | 0 (0) | 0 / 0 | 0 / 889 | 6 | 0.0% | 143 / 24 |
| rtt-20 | 20 ms RTT | 170 | 59 | 6.2 (1) | 31.7 / 52.2 / 73.7 | 1 (137) | 0 / 0 | 0 / 875 | 6 | 0.0% | 141 / 24 |
| rtt-50 | 50 ms RTT | 327 | 59 | 5.6 (2) | 55.3 / 81.9 / 108.5 | 1 (149) | 1 / 0 | 1 / 870 | 7 | 0.0% | 142 / 23 |
| rtt-100 | 100 ms RTT | 615 | 59 | 5.2 (3) | 73.7 / 94.2 / 116.7 | 1 (184) | 1 / 0 | 10 / 874 | 9 | 0.0% | 145 / 21 |
| rtt-200 | 200 ms RTT | 1472 | 56 | 4.2 (4) | 124.9 / 155.6 / 196.6 | 4 (982) | 4 / 42 | 32 / 854 | 14 | 0.0% | 143 / 19 |
| jitter | 50 ms RTT, ±10 ms jitter, in order | 455 | 53 | 1.7 (9) | 71.7 / 155.6 / 213.0 | 11 (2589) | 16 / 49 | 24 / 857 | 20 | 0.0% | 139 / 14 |
| reorder-2 | 50 ms RTT, 2% of packets 25 ms early | 329 | 58 | 3.4 (4) | 69.6 / 135.2 / 184.3 | 6 (1115) | 2 / 7 | 2 / 867 | 10 | 0.0% | 135 / 19 |
| reorder | 50 ms RTT, ±10 ms jitter, every packet on its own | 379 | 54 | 1.4 (12) | 64.5 / 167.9 / 221.2 | 12 (2560) | 24 / 49 | 27 / 830 | 20 | 0.1% | 129 / 14 |
| loss-1 | 50 ms RTT, 1% loss | 346 | 59 | 5.6 (2) | 48.1 / 98.3 / 135.2 | 3 (467) | 2 / 0 | 1 / 863 | 7 | 3.5% | 140 / 22 |
| loss-2 | 50 ms RTT, 2% loss | 329 | 60 | 5.6 (2) | 50.2 / 108.5 / 143.4 | 2 (265) | 1 / 0 | 1 / 876 | 7 | 5.5% | 143 / 22 |
| loss-5 | 50 ms RTT, 5% loss | 446 | 59 | 5.5 (2) | 59.4 / 110.6 / 151.6 | 3 (363) | 2 / 0 | 2 / 876 | 7 | 12.1% | 141 / 22 |
| loss-both | 50 ms RTT, 2% loss both ways | 332 | 59 | 5.5 (2) | 59.4 / 110.6 / 139.3 | 3 (619) | 2 / 0 | 1 / 869 | 7 | 5.7% | 142 / 22 |
| burst | 50 ms RTT, bursts of loss (~4%) | 328 | 59 | 5.6 (2) | 48.1 / 108.5 / 151.6 | 9 (1074) | 1 / 0 | 1 / 877 | 7 | 6.7% | 140 / 22 |
| cap-20 | 50 ms RTT, 20 Mbit/s, 100 ms buffer | 330 | 60 | 5.6 (1) | 50.2 / 73.7 / 102.4 | 1 (104) | 1 / 0 | 1 / 880 | 7 | 0.0% | 143 / 23 |
| cap-6 | 50 ms RTT, 6 Mbit/s (below the stream) | 341 | 59 | 3.8 (4) | 73.7 / 155.6 / 204.8 | 8 (997) | 1 / 0 | 1 / 873 | 15 | 0.0% | 143 / 19 |
| cap-drop | 50 ms RTT, 20 Mbit/s, 4 Mbit/s from 5 s to 10 s | 334 | 53 | 2.7 (5) | 51.2 / 159.7 / 225.3 | 7 (2324) | 18 / 80 | 18 / 874 | 18 | 2.1% | 142 / 16 |

Against the fixed 8 Mbit/s of the baseline:

| scenario | first picture, ms | fps | latency p50 / p95, ms | stalls (ms) |
|---|---:|---:|---:|---:|
| rtt-100, fixed → rate control | 941 → 615 | 58 → 59 | 74 / 92 → 74 / 94 | 2 (656) → 1 (184) |
| rtt-200 | 1993 → 1472 | 60 → 56 | 125 / 143 → 125 / 156 | 0 (0) → 4 (982) |
| cap-6 | 557 → 341 | 30 → 59 | 180 / 234 → 74 / 156 | 11 (7177) → 8 (997) |
| cap-drop (fixed: one run) | 496 → 334 | 40 → 53 | 61 / 156 → 51 / 160 | 5 (5072) → 7 (2324) |
| reorder | 548 → 379 | 6 → 54 | 121 / 217 → 65 / 168 | 2 (5566) → 12 (2560) |
| reorder-2 | 564 → 329 | 60 → 58 | 71 / 86 → 70 / 135 | 0 (0) → 6 (1115) |
| jitter | 594 → 455 | 51 → 53 | 86 / 152 → 72 / 156 | 2 (1005) → 11 (2589) |

- **A narrow path plays.** `cap-6` keeps 59 fps at ~4 Mbit/s where a fixed 8 Mbit/s played 30
  with half of every second stalled; when the path falls to 4 Mbit/s mid-stream (`cap-drop`) the
  bitrate is cut within one to two seconds, the viewer sees about two seconds of trouble rather
  than five, and the ramp is back at 8 Mbit/s four seconds after the path is.
- **Joining is faster** at every distance (0.33 s at 50 ms RTT, 0.6 s at 100 ms, 1.5 s at 200 ms
  against 0.5 / 0.9 / 2.0 s): the first keyframe is a quarter of the size.
- **Clean and lossy paths are unchanged** once the ramp is done (5–7 s to reach 8 Mbit/s; the
  first cut is the start's expired keyframe, librats L2); loss is not taken for congestion.
  `burst` stalls more often (9 against 4–8), in the same short stalls a lost reference costs.
- **Where BBR's estimate is wrong, rate control follows it down.** On `jitter` BBR's Startup
  exits on the jitter and leaves an estimate of 1–4 Mbit/s on a 1 Gbit/s path; filled with
  keyframes that estimate is a full path, and the stream settles at 1–2 Mbit/s with more, shorter
  stalls than a fixed 8 Mbit/s that kept BBR busy enough to grow its estimate. `reorder-2` loses
  similarly to BBR's window (25 ms round trips from the early packets: the sender queues), and
  `reorder` plays for the first time (54 fps) but at 1.4 Mbit/s. These are librats' to fix
  (`PLAN.md`, librats track, L2 and L7), not rate control's to work around: the estimate is
  all a sender has to go by. Fixed since in librats (L2 (c)), below.

### After librats L2 (c)

librats at `19d711b`: BBR now checks a queue it reads off a jittery round trip by padding up to
its pacing rate before believing it, sizes its window over the round trip the path shows at
load rather than over one lucky minimum, ends a probe only on a burst later than twice the
path's own ack lateness, and no longer reads random loss on a path that holds packets back as a
full queue. The same runs, rate-controlled (`bench/netem.py --repeat 3`):

1920x1080 at 60 fps, AV1, at most 8 Mbit/s, frame lifetime 200 ms; 15 s a run, medians of 3.

| scenario | path | first picture, ms | fps | Mbit/s (cuts) | latency p50 / p95 / p99, ms | stalls (ms) | lost / dropped | expired / sent | keyframes | repair | CPU send / recv, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| local | veth, no shaping | 34 | 60 | 6.3 (1) | 15.9 / 29.7 / 37.9 | 0 (0) | 0 / 0 | 0 / 889 | 6 | 0.0% | 143 / 23 |
| rtt-20 | 20 ms RTT | 168 | 60 | 6.2 (1) | 29.2 / 46.1 / 67.6 | 0 (0) | 0 / 0 | 0 / 888 | 6 | 0.0% | 140 / 24 |
| rtt-50 | 50 ms RTT | 327 | 60 | 5.7 (2) | 47.1 / 65.5 / 90.1 | 0 (0) | 1 / 0 | 1 / 886 | 7 | 0.0% | 142 / 22 |
| rtt-100 | 100 ms RTT | 823 | 60 | 5.3 (2) | 71.7 / 92.2 / 114.7 | 0 (0) | 0 / 3 | 10 / 879 | 9 | 0.0% | 143 / 21 |
| rtt-200 | 200 ms RTT | 1628 | 60 | 4.7 (3) | 122.9 / 147.5 / 180.2 | 0 (0) | 0 / 15 | 32 / 862 | 11 | 0.0% | 142 / 20 |
| jitter | 50 ms RTT, ±10 ms jitter, in order | 340 | 60 | 5.7 (2) | 57.3 / 88.1 / 167.9 | 2 (233) | 1 / 0 | 1 / 889 | 7 | 0.0% | 141 / 22 |
| reorder-2 | 50 ms RTT, 2% of packets 25 ms early | 329 | 60 | 5.6 (2) | 47.1 / 67.6 / 98.3 | 0 (0) | 1 / 0 | 1 / 882 | 7 | 0.0% | 143 / 23 |
| reorder | 50 ms RTT, ±10 ms jitter, every packet on its own | 331 | 59 | 5.3 (3) | 60.4 / 88.1 / 151.6 | 2 (472) | 5 / 0 | 7 / 883 | 10 | 0.0% | 142 / 21 |
| loss-1 | 50 ms RTT, 1% loss | 329 | 60 | 5.6 (1) | 49.2 / 98.3 / 131.1 | 4 (453) | 1 / 0 | 1 / 882 | 7 | 3.0% | 144 / 23 |
| loss-2 | 50 ms RTT, 2% loss | 333 | 60 | 5.7 (2) | 50.2 / 106.5 / 127.0 | 3 (392) | 1 / 0 | 1 / 883 | 7 | 5.6% | 143 / 22 |
| loss-5 | 50 ms RTT, 5% loss | 342 | 60 | 5.7 (2) | 59.4 / 110.6 / 155.6 | 6 (777) | 3 / 0 | 2 / 884 | 7 | 11.3% | 140 / 22 |
| loss-both | 50 ms RTT, 2% loss both ways | 329 | 60 | 5.7 (2) | 49.2 / 104.4 / 139.3 | 3 (342) | 1 / 0 | 1 / 888 | 7 | 5.6% | 142 / 23 |
| burst | 50 ms RTT, bursts of loss (~4%) | 330 | 59 | 5.6 (1) | 47.1 / 102.4 / 151.6 | 5 (625) | 2 / 0 | 1 / 886 | 7 | 6.0% | 143 / 23 |
| cap-20 | 50 ms RTT, 20 Mbit/s, 100 ms buffer | 337 | 60 | 5.7 (2) | 53.2 / 73.7 / 100.4 | 0 (0) | 1 / 0 | 1 / 885 | 7 | 0.0% | 156 / 26 |
| cap-6 | 50 ms RTT, 6 Mbit/s (below the stream) | 345 | 60 | 3.8 (4) | 75.8 / 151.6 / 196.6 | 7 (789) | 1 / 0 | 1 / 880 | 13 | 0.0% | 157 / 23 |
| cap-drop | 50 ms RTT, 20 Mbit/s, 4 Mbit/s from 5 s to 10 s | 334 | 50 | 2.8 (7) | 61.4 / 188.4 / 245.8 | 11 (2921) | 32 / 84 | 32 / 879 | 19 | 1.3% | 156 / 18 |

Against the runs above (rate-controlled, before L2 (c)):

| scenario | fps | Mbit/s (cuts) | latency p50 / p95, ms | stalls (ms) |
|---|---:|---:|---:|---:|
| jitter | 53 → 60 | 1.7 (9) → 5.7 (2) | 72 / 156 → 57 / 88 | 11 (2589) → 2 (233) |
| reorder | 54 → 59 | 1.4 (12) → 5.3 (3) | 65 / 168 → 60 / 88 | 12 (2560) → 2 (472) |
| reorder-2 | 58 → 60 | 3.4 (4) → 5.6 (2) | 70 / 135 → 47 / 68 | 6 (1115) → 0 (0) |
| rtt-200 | 56 → 60 | 4.2 (4) → 4.7 (3) | 125 / 156 → 123 / 148 | 4 (982) → 0 (0) |
| cap-6 | 59 → 60 | 3.8 (4) → 3.8 (4) | 74 / 156 → 76 / 152 | 8 (997) → 7 (789) |
| cap-drop | 53 → 50 | 2.7 (5) → 2.8 (7) | 51 / 160 → 61 / 188 | 7 (2324) → 11 (2921) |

- **Jittery and reordering paths play like clean ones.** `jitter`, `reorder` and `reorder-2`
  ramp and hold as `rtt-50` does (5.3–5.7 Mbit/s over a run, ramp included; two or three cuts,
  the start's among them), at 59–60 fps with p95 under 90 ms. `reorder`, per-packet jitter
  several times the base round trip, played nothing in the baseline. Every scenario now plays
  at least as well rate-controlled as at a fixed 8 Mbit/s.
- **Clean and lossy paths are unchanged**, within run-to-run spread.
- **The start still costs a rate cut.** The joining keyframe expires behind the initial window
  (L2 (a, b)) and rate control takes it for congestion: 2 → 1.7 Mbit/s at 50 ms RTT, to
  1.2 Mbit/s at 200 ms (32 frames expired before the first picture), then a 2 s hold, so a
  stream reaches 8 Mbit/s 7 s in at 50 ms RTT and 9 s in at 200 ms rather than ~4 s.
- **The first picture is a little later on long paths.** 0.33 s at 50 ms RTT as before; at
  100 ms it lands, as before, on one of two values one keyframe request apart (~0.62 or
  ~0.83 s), now the later in two runs of three; at 200 ms it comes 1.59–1.67 s in, where it
  came 1.47 s in (two runs of three before, the third took 4.6 s). The start is L2 (a, b)'s
  and 1.2's to fix.
- **`cap-drop` is a little worse**: 50 fps and 7 cuts against 53 and 5, the stalls 2.9 s
  against 2.3. Either spread or rate control swinging on an estimate that now recovers faster;
  to look at over more runs (`PLAN.md`, 1.1).

### The start and a narrowing path

Two changes to rate control (`PLAN.md`, 1.1), measured on the per-sample path figures:

- **The transport's start is not overuse.** The start's cut came from the joining keyframe's
  frames expiring while BBR was still in Startup — pacing at 2.74× an estimate that is only how
  far it got (4.7 Mbit/s on a gigabit veth at 50 ms RTT; at 200 ms, 0.01 Mbit/s, which made a
  24 KB queue read as 3.7 s). While a sample paces at more than twice its estimate, expiries and
  the queue no longer count; a full queue still does. A stream that overfills the path ends
  Startup on its own within a few round trips (`cap-drop` did, 0.1 s before its first expiry).
- **A cut goes by what the path delivered.** When the path fell from 20 to 4 Mbit/s, BBR's
  estimate — the best of the last few seconds — took 3.5 s to come down (10.7 → 7.5 → 5.2 →
  4.0 Mbit/s), so each cut took 15% off and the stream stepped down for 2–3 s. What the path
  delivered in the 100 ms between two busy samples said 3.7–4.1 Mbit/s from the first one: a cut
  now goes to 85% of the lesser of the two.

Without the start's cut a third change was needed: that cut's new encoder had brought a keyframe
by chance, and without it the frame expired behind the joining keyframe cost every run a stall of
~0.3 s (21 frames dropped) — the viewer asked for a keyframe soon after the last one, which the
sender ignores, and asked again 250 ms later. Now the sender takes expiries on a subscriber's
path for a keyframe needed at once (held to `min_keyframe_interval`), and an encoder that opens
another for a new bitrate lets the one keyframe it starts with answer a request made meanwhile.

1920x1080 at 60 fps, AV1, at most 8 Mbit/s, frame lifetime 200 ms; 15 s a run, medians of 3.

| scenario | path | first picture, ms | fps | Mbit/s (cuts) | latency p50 / p95 / p99, ms | stalls (ms) | lost / dropped | expired / sent | keyframes | repair | CPU send / recv, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| local | veth, no shaping | 34 | 60 | 6.3 (0) | 16.4 / 30.2 / 39.9 | 0 (0) | 0 / 0 | 0 / 890 | 6 | 0.0% | 144 / 23 |
| rtt-20 | 20 ms RTT | 153 | 60 | 6.3 (0) | 29.7 / 46.1 / 62.5 | 0 (0) | 0 / 0 | 0 / 891 | 6 | 0.0% | 143 / 24 |
| rtt-50 | 50 ms RTT | 328 | 60 | 6.2 (0) | 48.1 / 67.6 / 86.0 | 1 (101) | 1 / 0 | 1 / 883 | 7 | 0.0% | 146 / 24 |
| rtt-100 | 100 ms RTT | 681 | 60 | 6.1 (0) | 73.7 / 92.2 / 114.7 | 1 (113) | 1 / 0 | 11 / 879 | 8 | 0.0% | 148 / 23 |
| rtt-200 | 200 ms RTT | 1448 | 59 | 5.9 (0) | 124.9 / 147.5 / 180.2 | 1 (191) | 3 / 14 | 31 / 864 | 10 | 0.0% | 148 / 22 |
| jitter | 50 ms RTT, ±10 ms jitter, in order | 396 | 60 | 6.2 (0) | 58.4 / 90.1 / 127.0 | 1 (119) | 1 / 0 | 3 / 882 | 7 | 0.0% | 146 / 23 |
| reorder-2 | 50 ms RTT, 2% of packets 25 ms early | 306 | 60 | 6.2 (0) | 46.1 / 65.5 / 86.0 | 1 (113) | 1 / 0 | 1 / 888 | 7 | 0.0% | 144 / 24 |
| reorder | 50 ms RTT, ±10 ms jitter, every packet on its own | 370 | 58 | 5.9 (1) | 65.5 / 92.2 / 108.5 | 2 (499) | 11 / 0 | 11 / 885 | 9 | 0.0% | 150 / 23 |
| loss-1 | 50 ms RTT, 1% loss | 328 | 60 | 6.2 (0) | 48.1 / 100.4 / 124.9 | 3 (334) | 1 / 0 | 1 / 888 | 7 | 3.6% | 149 / 24 |
| loss-2 | 50 ms RTT, 2% loss | 330 | 59 | 6.2 (0) | 49.2 / 102.4 / 116.7 | 3 (342) | 1 / 0 | 2 / 883 | 7 | 5.1% | 146 / 23 |
| loss-5 | 50 ms RTT, 5% loss | 332 | 59 | 6.2 (0) | 59.4 / 110.6 / 139.3 | 6 (747) | 1 / 0 | 1 / 886 | 7 | 10.7% | 146 / 23 |
| loss-both | 50 ms RTT, 2% loss both ways | 329 | 60 | 6.2 (0) | 50.2 / 106.5 / 122.9 | 5 (571) | 1 / 0 | 1 / 881 | 7 | 5.9% | 147 / 24 |
| burst | 50 ms RTT, bursts of loss (~4%) | 328 | 60 | 6.2 (0) | 47.1 / 112.6 / 163.8 | 8 (1005) | 2 / 0 | 1 / 889 | 7 | 8.1% | 145 / 23 |
| cap-20 | 50 ms RTT, 20 Mbit/s, 100 ms buffer | 332 | 60 | 6.2 (0) | 51.2 / 71.7 / 92.2 | 0 (0) | 1 / 0 | 1 / 889 | 7 | 0.0% | 146 / 24 |
| cap-6 | 50 ms RTT, 6 Mbit/s (below the stream) | 341 | 60 | 4.0 (2) | 73.7 / 147.5 / 184.3 | 8 (860) | 1 / 0 | 1 / 886 | 13 | 0.0% | 147 / 20 |
| cap-drop | 50 ms RTT, 20 Mbit/s, 4 Mbit/s from 5 s to 10 s | 330 | 55 | 3.2 (5) | 54.3 / 172.0 / 217.1 | 9 (1499) | 29 / 22 | 29 / 884 | 21 | 0.9% | 146 / 18 |

Against the runs after L2 (c), above:

| scenario | Mbit/s (cuts) | fps | latency p95, ms | stalls (ms) | keyframes |
|---|---:|---:|---:|---:|---:|
| rtt-50 | 5.7 (2) → 6.2 (0) | 60 → 60 | 66 → 68 | 0 (0) → 1 (101) | 7 → 7 |
| rtt-200 | 4.7 (3) → 5.9 (0) | 60 → 59 | 148 → 148 | 0 (0) → 1 (191) | 11 → 10 |
| jitter | 5.7 (2) → 6.2 (0) | 60 → 60 | 88 → 90 | 2 (233) → 1 (119) | 7 → 7 |
| loss-2 | 5.7 (2) → 6.2 (0) | 60 → 59 | 107 → 102 | 3 (392) → 3 (342) | 7 → 7 |
| cap-6 | 3.8 (4) → 4.0 (2) | 60 → 60 | 152 → 148 | 7 (789) → 8 (860) | 13 → 13 |
| cap-drop | 2.8 (7) → 3.2 (5) | 50 → 55 | 188 → 172 | 11 (2921) → 9 (1499) | 19 → 21 |

(The cuts now leave out the return to the start bitrate when the viewer leaves at the end of a
run, which the column above counted as one.)

- **No cut at the start**: every path but the narrow ones ramps 2 → 8 Mbit/s in four seconds
  and stays there, 5.9–6.3 Mbit/s over a run against 4.7–5.7.
- **The start costs a short stall instead**: one of 0.1–0.2 s right after the first picture on
  most paths, the frame behind the joining keyframe expiring (L2 (b)) and the keyframe that
  replaces it held back by `min_keyframe_interval` (200 ms after the first). 1.2's joining and
  L2 (b) are what remove it.
- **A narrowing path is followed in one cut**: `cap-drop` plays at 55 fps with 1.5 s of stalls
  in its five seconds at 4 Mbit/s (2.9 s before, 5.1 s at a fixed 8 Mbit/s).
- `cap-6` is unchanged within its spread (3–10 stalls a run): its stalls are the keyframes of
  each change on a path with no room for them (13 in 15 s) — 1.1's fewer keyframes.

## Playout buffer (1.3)

Most stalls on lossy paths were frames that came late, not frames lost: a packet FEC did not
rebuild is sent again a round trip later, and the frames behind it wait for it in decode order.
In the runs above, nearly every second with a stall on `loss-*` and `burst` had no frame lost,
late or dropped, and its worst latency was 130–220 ms against ~55 ms. The player now shows each
picture on a schedule (`PlayoutSchedule`, `PLAN.md` 1.3) that covers how late frames come.

Measured on the way: frames reach the viewer in clumps every 66 ms on every path, `local`
included — SVT-AV1's low-delay structure makes every fourth frame ~30 KB against 4–15 KB for
the others and ~12 ms slower to encode, and the three behind it wait. Shown as they come, that
is motion that judders (a window shows the newest picture, so two of a clump never reach the
screen); the buffer evens it out for ~20 ms.

One run each (`bench/netem.py local rtt-50 loss-2 burst`, the default buffer: at most the frame
lifetime), against the last full table above; medians of three runs are to be taken when stage 1
is measured as a whole:

| scenario | latency p50 / p95, ms | stalls (ms) | buffer at the end, ms |
|---|---:|---:|---:|
| local | 16 / 30 → 34 / 36 | 0 → 0 | 23 |
| rtt-50 | 48 / 68 → 66 / 70 | 1 (101) → 1 (102) | 30 |
| loss-2 | 49 / 102 → 111 / 119 | 3 (342) → 2 (217) | 75 |
| burst | 47 / 113 → 125 / 135 | 8 (1005) → 1 (318) | 91 |

The stalls left are the start's (the frame expired behind the joining keyframe, 1.2's).

## Joining (1.2, with librats L3)

Where the wait for the first picture went, read off timestamps on both ends of one run each
(`rtt-100`, `rtt-200`; `PLAN.md` 1.2's notes): two round trips to subscribe, half one for the
sender to hear of it, and then keyframes expiring behind a fresh connection's 4-packet window —
2 sent at 100 ms RTT, 6 at 200 ms, before one arrived. Now the joining keyframe and the frames
behind it are let arrive late (four round trips more than the lifetime, shrinking back), and the
sender hears what became of each frame (L3) instead of reading the path's expiry count.

One run each, 6 s (`bench/netem.py --duration 6 …`), against the last full table above; medians
of three when stage 1 is measured as a whole:

| scenario | first picture, ms | stalls (ms) | expired / keyframes |
|---|---:|---:|---:|
| rtt-50 | 328 → 328 | 1 (101) → 0 | 1 / 7 → 0 / 6 |
| rtt-100 | 681 → 617 | 1 (113) → 1 (136) | 11 / 8 → 1 / 7 |
| rtt-200 | 1448 → 1176 | 1 (191) → 2 (414) | 31 / 10 → 8 / 10 |
| loss-2 | 330 → 323 | 3 (342) → 1 (129) | 2 / 7 → 0 / 6 |
| burst | 328 → 382 | 8 (1005) → 1 (100) | 1 / 7 → 1 / 7 |

(The `loss-2` and `burst` stalls fell with 1.3; the first picture there stays ~330 ms, 323–384 ms
over six runs of `loss-2` — the higher ones a keyframe repaired on the way. One `rtt-50` run had
two stalls of 0.4 s with the sender's own frame rate down to 47 in the same seconds: the VM, not
the path — the two runs after it had none.) Left at 200 ms RTT: the keyframe
still needs ~3 round trips through the 4-packet window, and BBR's Startup paces at its floor
meanwhile, by which librats sheds the frames behind it — the stall after the first picture is
the keyframe that has to follow (L2 (a, b)).


## Stage 1, measured whole

After 1.1, 1.2 (with librats L3) and 1.3: ratcast 694f0b7, librats 3548423.
`bench/netem.py --repeat 3`, rate-controlled (the default: 2 Mbit/s up to 8):

| scenario | path | first picture, ms | fps | Mbit/s (cuts) | latency p50 / p95 / p99, ms | stalls (ms) | lost / dropped | expired / sent | keyframes | repair | CPU send / recv, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| local | veth, no shaping | 36 | 60 | 6.3 (0) | 29.7 / 32.3 / 37.9 | 0 (0) | 0 / 0 | 0 / 891 | 6 | 0.0% | 142 / 24 |
| rtt-20 | 20 ms RTT | 154 | 60 | 6.3 (0) | 45.1 / 48.1 / 64.5 | 0 (0) | 0 / 0 | 0 / 894 | 6 | 0.0% | 142 / 24 |
| rtt-50 | 50 ms RTT | 328 | 60 | 6.2 (0) | 65.5 / 69.6 / 151.6 | 0 (0) | 0 / 0 | 0 / 886 | 6 | 0.0% | 144 / 24 |
| rtt-100 | 100 ms RTT | 616 | 60 | 6.2 (0) | 90.1 / 94.2 / 147.5 | 1 (147) | 2 / 0 | 2 / 881 | 8 | 0.0% | 144 / 23 |
| rtt-200 | 200 ms RTT | 1178 | 59 | 6.0 (0) | 143.4 / 147.5 / 393.2 | 1 (419) | 9 / 17 | 9 / 865 | 10 | 0.0% | 142 / 22 |
| jitter | 50 ms RTT, ±10 ms jitter, in order | 385 | 60 | 6.2 (0) | 60.4 / 84.0 / 208.9 | 1 (142) | 0 / 0 | 0 / 881 | 6 | 0.0% | 141 / 23 |
| reorder-2 | 50 ms RTT, 2% of packets 25 ms early | 334 | 60 | 6.2 (0) | 49.2 / 71.7 / 172.0 | 0 (0) | 0 / 0 | 0 / 889 | 6 | 0.0% | 144 / 23 |
| reorder | 50 ms RTT, ±10 ms jitter, every packet on its own | 322 | 58 | 5.6 (2) | 67.6 / 86.0 / 188.4 | 3 (574) | 9 / 0 | 9 / 885 | 10 | 0.0% | 142 / 22 |
| loss-1 | 50 ms RTT, 1% loss | 333 | 60 | 6.2 (0) | 110.6 / 118.8 / 167.9 | 0 (0) | 0 / 0 | 0 / 882 | 6 | 3.3% | 141 / 24 |
| loss-2 | 50 ms RTT, 2% loss | 330 | 60 | 6.2 (0) | 108.5 / 118.8 / 151.6 | 0 (0) | 0 / 0 | 0 / 889 | 6 | 5.1% | 142 / 24 |
| loss-5 | 50 ms RTT, 5% loss | 365 | 60 | 6.2 (0) | 155.6 / 155.6 / 213.0 | 0 (0) | 1 / 0 | 0 / 888 | 6 | 10.7% | 144 / 24 |
| loss-both | 50 ms RTT, 2% loss both ways | 393 | 60 | 6.1 (0) | 114.7 / 129.0 / 233.5 | 1 (116) | 0 / 0 | 0 / 877 | 6 | 5.2% | 141 / 23 |
| burst | 50 ms RTT, bursts of loss (~4%) | 325 | 60 | 6.2 (0) | 122.9 / 129.0 / 155.6 | 0 (0) | 0 / 0 | 0 / 884 | 6 | 7.8% | 141 / 24 |
| cap-20 | 50 ms RTT, 20 Mbit/s, 100 ms buffer | 329 | 60 | 6.2 (0) | 69.6 / 73.7 / 151.6 | 0 (0) | 0 / 0 | 0 / 881 | 6 | 0.0% | 143 / 24 |
| cap-6 | 50 ms RTT, 6 Mbit/s (below the stream) | 337 | 60 | 3.9 (2) | 92.2 / 159.7 / 204.8 | 1 (125) | 0 / 0 | 0 / 881 | 13 | 0.0% | 142 / 20 |
| cap-drop | 50 ms RTT, 20 Mbit/s, 4 Mbit/s from 5 s to 10 s | 329 | 52 | 3.2 (6) | 79.9 / 192.5 / 270.3 | 10 (2454) | 48 / 41 | 48 / 881 | 24 | 1.4% | 143 / 17 |

The same at a fixed 8 Mbit/s (`--send-arg=--fixed-bitrate`):

| scenario | path | first picture, ms | fps | Mbit/s (cuts) | latency p50 / p95 / p99, ms | stalls (ms) | lost / dropped | expired / sent | keyframes | repair | CPU send / recv, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| local | veth, no shaping | 47 | 60 | 7.5 (0) | 27.6 / 29.7 / 32.3 | 0 (0) | 0 / 0 | 0 / 896 | 2 | 0.0% | 143 / 25 |
| rtt-20 | 20 ms RTT | 206 | 60 | 7.5 (0) | 43.0 / 46.1 / 48.1 | 0 (0) | 0 / 0 | 0 / 894 | 2 | 0.0% | 143 / 25 |
| rtt-50 | 50 ms RTT | 412 | 61 | 7.4 (0) | 58.4 / 62.5 / 192.5 | 0 (0) | 0 / 0 | 0 / 890 | 2 | 0.0% | 144 / 26 |
| rtt-100 | 100 ms RTT | 762 | 60 | 7.2 (0) | 88.1 / 90.1 / 147.5 | 1 (182) | 10 / 0 | 10 / 882 | 4 | 0.0% | 145 / 24 |
| rtt-200 | 200 ms RTT | 1468 | 59 | 6.8 (0) | 139.3 / 143.4 / 344.1 | 1 (402) | 32 / 15 | 32 / 867 | 7 | 0.0% | 146 / 23 |
| jitter | 50 ms RTT, ±10 ms jitter, in order | 457 | 60 | 7.3 (0) | 84.0 / 86.0 / 200.7 | 1 (198) | 1 / 0 | 1 / 888 | 3 | 0.0% | 145 / 24 |
| reorder-2 | 50 ms RTT, 2% of packets 25 ms early | 409 | 60 | 7.4 (0) | 55.3 / 60.4 / 180.2 | 1 (142) | 1 / 0 | 1 / 882 | 3 | 0.1% | 143 / 25 |
| reorder | 50 ms RTT, ±10 ms jitter, every packet on its own | 422 | 60 | 7.4 (0) | 79.9 / 86.0 / 233.5 | 0 (0) | 0 / 0 | 0 / 891 | 2 | 0.0% | 146 / 25 |
| loss-1 | 50 ms RTT, 1% loss | 404 | 60 | 7.4 (0) | 108.5 / 116.7 / 120.8 | 0 (0) | 1 / 0 | 1 / 887 | 3 | 2.7% | 144 / 26 |
| loss-2 | 50 ms RTT, 2% loss | 414 | 60 | 7.4 (0) | 108.5 / 118.8 / 135.2 | 0 (0) | 1 / 0 | 1 / 890 | 3 | 5.5% | 146 / 26 |
| loss-5 | 50 ms RTT, 5% loss | 453 | 60 | 7.3 (0) | 116.7 / 167.9 / 188.4 | 1 (128) | 2 / 0 | 3 / 884 | 3 | 10.6% | 145 / 25 |
| loss-both | 50 ms RTT, 2% loss both ways | 413 | 60 | 7.4 (0) | 110.6 / 120.8 / 192.5 | 0 (0) | 1 / 0 | 1 / 890 | 3 | 5.6% | 143 / 25 |
| burst | 50 ms RTT, bursts of loss (~4%) | 402 | 60 | 7.4 (0) | 139.3 / 159.7 / 180.2 | 1 (151) | 1 / 0 | 1 / 884 | 3 | 9.0% | 147 / 26 |
| cap-20 | 50 ms RTT, 20 Mbit/s, 100 ms buffer | 414 | 60 | 7.4 (0) | 71.7 / 75.8 / 86.0 | 0 (0) | 1 / 0 | 1 / 888 | 3 | 0.0% | 145 / 25 |
| cap-6 | 50 ms RTT, 6 Mbit/s (below the stream) | 420 | 40 | 5.3 (0) | 163.8 / 221.2 / 254.0 | 17 (4770) | 53 / 210 | 53 / 882 | 36 | 0.0% | 155 / 18 |
| cap-drop | 50 ms RTT, 20 Mbit/s, 4 Mbit/s from 5 s to 10 s | 412 | 39 | 5.5 (0) | 73.7 / 217.1 / 360.4 | 4 (4956) | 129 / 180 | 129 / 885 | 25 | 1.0% | 151 / 18 |

Against the stage's *done when* (`PLAN.md`, stage 1):

- **≥ 55 fps everywhere but `cap-drop`**: met — 58–60 fps, `reorder` the lowest.
- **`cap-drop` back to full rate within 2 s of each change**: met when the path widens again,
  not when it narrows. After the fall to 4 Mbit/s the picture runs at 12–40 fps for about
  three seconds (seconds 6–8 of every run) before it is back at 60: each of the 4–7 cuts is a new
  encoder and a keyframe, on a path that has no room for them (21–25 keyframes a run).
- **First picture within about four round trips + 150 ms, no stall after it**: met up to 50 ms
  RTT (154 ms at 20, 328 at 50, no stall). Not at 100 ms (616 against ~550, then a 0.15 s stall)
  or 200 ms (1178 against ~950, then 0.4 s): librats L2 (a, b), put off.
- **No scenario worse with rate control than at a fixed bitrate**: met everywhere but `reorder`.
  There rate control loses a few frames at its bitrate changes, 9–15 in two runs of three (58 fps,
  0.6 s of stalls), where the fixed stream loses none. Every change is a keyframe on a path that
  reorders every packet. Rate control wins clearly where the path is narrow: `cap-6` 40 → 60 fps,
  4.8 → 0.1 s of stalls; `cap-drop` 39 → 52 fps, 5.0 → 2.5 s. It also starts sooner everywhere,
  with a smaller first keyframe: 412 → 328 ms at 50 ms RTT.

The `cap-drop` row above is the unluckiest of several series. Built from c35002f (before 1.2;
librats 19d711b, before L3) and run against this code on the same machine, two series of three
each: `cap-drop` old 52 / 51 fps with 2.3 / 2.6 s of stalls, new 56 / 56 fps with 1.2 / 1.4 s;
`reorder` old 59 / 58 fps with 0.3 / 0.4 s, new 59 / 59 fps with 0.4 / 0.7 s, the same within
its spread. The rows that moved since the last full table: `loss-*` and `burst` (1.3), 0–1
stalls where there were 3–8, at ~110–155 ms p50 for ~50; `rtt-100` and `rtt-200` (1.2), first
picture 681 → 616 and 1448 → 1178 ms.

What is left, in both cases: fewer keyframes at bitrate changes (1.1's leftover — on `reorder`
and `cap-drop`), and librats L2 (a, b) for joining on long paths.

## Opus (2.1)

`bench_codec`, its last rows: ten seconds of a chord with some noise over it (a tone alone Opus
codes in next to nothing), encoded and decoded in 20 ms frames, libopus 1.6.1 at its default
complexity (10).

| | bitrate | encode | decode | frame |
|---|---:|---:|---:|---:|
| voice: mono, in-band FEC for 10% loss | 32 kbit/s | 116 µs | 28 µs | 77 B |
| music: stereo | 128 kbit/s | 113 µs | 31 µs | 322 B |

Under 0.6% of a core to encode, 0.15% to decode: sound costs nothing next to the picture (the
1080p60 sender of the stage 1 runs takes ~140% of a core), so the complexity stays at its best.

## Sound over the network (2.1)

`bench/netem.py --audio --keyframe-every 2 --duration 10`: the stamped 1080p60 pattern as before,
with a stamped tone beside it (Opus voice, 32 kbit/s, DTX, FEC for 10% loss) and a keyframe every
2 s on top of those asked for, so the sound keeps meeting keyframe bursts. The viewer plays the
sound in real time into an output without a device (10 ms blocks, 10 ms of "device" latency).
Single runs while 2.1 is built; the stage is measured whole at its end.

| scenario | first sound, ms | latency p50 / p95 / p99 / max, ms | jitter buffer delay / target, ms | lost / late | FEC / concealed, ms | breaks |
|---|---:|---:|---:|---:|---:|---:|
| local | 35 | 56.3 / 57.3 / 58.4 / 59.7 | 26 / 24 | 0 / 0 | 0 / 0 | 0 |
| loss-2 | 198 | 98.3 / 100.2 / 100.2 / 100.2 | 44 / 26 | 11 / 11 | 200 / 40 | 0 |
| cap-20 | 197 | 79.9 / 79.9 / 79.9 / 81.8 | 22 / 30 | 2 / 2 | 0 / 40 | 0 |
| cap-6 | 199 | 100.4 / 120.8 / 120.8 / 263.9 | 64 / 64 | 8 / 8 | 0 / 200 | 1 |

Latency is from the tone's capture to the speaker. A break is 100 ms or more without a frame
played.

- **A clean path, 56 ms**: 20 ms to fill a frame (its capture time is its first sample's), the
  jitter buffer's 24 ms (2 ms of lateness and a frame, so that the next frame's FEC is there for
  a lost one), 10 ms of output, and the output's blocks. The frame for FEC is the place to look
  when 2.3's `call` profile wants less.
- **2% loss, no break**: 11 frames lost on the wire. librats repaired each of them, but the
  repair came a round trip later, after its time, so the frames count as late. The next frame's
  FEC brought back 10 of them; 40 ms was concealed over 10 s.
- **Next to a keyframe (cap-20)**: the frames late are the first second's only, while the
  viewer joins and the target is still learnt. The keyframes every 2 s cost the sound nothing.
- **A path narrower than the stream (cap-6)** is where it breaks. At 6 s the video, which rate
  control had let up to 4.85 Mbit/s of 6, met a keyframe and filled the bottleneck's queue. The
  sound waits in that queue like everything else (netem's FIFO, which no rank in the sender
  overtakes): 7 frames late in that second, a 140 ms break. The video lost 8 frames there too.
  This is rate control's margin, not the sound's.

L4, measured by turning it off (every live message rank 0, so deadline order only), on the same
paths:

| scenario | latency p50 / p95 / max, ms, ranked → not | lost / late, ranked → not | concealed, ms, ranked → not |
|---|---:|---:|---:|
| local | 56.3 / 57.3 / 59.7 → 55.3 / 56.3 / 57.5 | 0 → 0 | 0 → 0 |
| cap-20 | 79.9 / 79.9 / 81.8 → 79.9 / 79.9 / 81.2 | 2 → 1 | 40 → 20 |
| cap-6 | 100.4 / 120.8 / 263.9 → 180.2 / 200.5 / 200.5 | 8 → 16 | 200 → 440 |

Where librats' own queue has room, ranks change nothing. Where the stream fills it (`cap-6`),
the sound queues behind the video without ranks: 80 ms more latency at the median, twice the
frames lost, and the jitter buffer pushed to its 200 ms limit.

Opus codes FEC only for frames its voice activity detector (SILK's) takes for speech. A steady
tone qualifies only above about 0.45 of full scale, which is why the senders' tone plays at 0.5.
A real voice qualifies as it is.

## Lip sync (2.2)

The same runs with the pictures and the sound played in step (`PlayoutClock`), and two of them
with `--recv-arg=--no-sync` beside them. "Apart" is, for each picture shown, how far it is from
the sound captured with it, either way. The mean, with its sign, is positive when the pictures
are late. Single runs, a keyframe every 2 s.

| scenario | apart p50 / p95 / p99 / max (mean), ms | sound latency p50, ms | picture latency p50 / p95, ms | jitter buffer delay, ms |
|---|---:|---:|---:|---:|
| local, not in step | 24.6 / 28.7 / 42.0 / 42.0 (−24.3) | 55.3 | 30.7 / 34.8 | 24 |
| local | 0.5 / 1.6 / 4.6 / 7.7 (+0.5) | 57.3 | 57.3 / 58.4 | 26 |
| loss-2, not in step | 14.3 / 44.0 / 122.9 / 151.4 (−4.4) | 98.3 | 108.5 / 116.7 | 42 |
| loss-2 | 1.9 / 11.8 / 143.4 / 171.6 (+5.7) | 108.5 | 110.6 / 129.0 | 72 |
| jitter | 0.6 / 7.9 / 131.1 / 196.1 (+4.6) | 96.3 | 98.3 / 102.4 | 65 |
| burst | 0.9 / 15.9 / 108.5 / 138.1 (+5.2) | 100.4 | 104.4 / 143.4 | 84 |

- **In step to within a few milliseconds** at the median, 16 at p95, on every path tried. Not in
  step, the pictures ran 24 ms ahead of the sound on a clean path, and 44 ms apart at p95 on a
  lossy one.
- **The tail is the pictures' own lateness.** The 1% that are 100+ ms apart are pictures behind a
  frame sent again (a late run the playout schedule chose not to cover) or the joining keyframe.
  They are as late without lip sync. Covering them would hold the sound for the worst moment of
  the pictures.
- **What it costs.** On a clean path the pictures wait for the sound, 31 → 57 ms. The sound's
  56 ms (a frame to fill, a frame for FEC) now sets the latency of both, so 2.3's `call` and
  `game` profiles have to start from it. On lossy paths the sound waits for the pictures
  instead: its jitter buffer is held at what they need (`loss-2` 42 → 72 ms, 98 → 108 ms from
  capture to speaker), reached by making up a frame now and then (40–60 ms over a run).

## Stage 2, measured whole

After 2.1 and 2.2: ratcast b5ab89a, librats 2141d8b. `bench/netem.py --audio
--repeat 3`: the pattern, rate-controlled (2 Mbit/s up to 8), with the stamped tone beside it,
played in step.

| scenario | path | first picture, ms | fps | Mbit/s (cuts) | latency p50 / p95 / p99, ms | stalls (ms) | lost / dropped | expired / sent | keyframes | repair | CPU send / recv, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| local | veth, no shaping | 55 | 60 | 6.3 (0) | 57.3 / 60.4 / 61.4 | 0 (0) | 0 / 0 | 0 / 1639 | 6 | 0.0% | 146 / 25 |
| rtt-20 | 20 ms RTT | 169 | 60 | 6.3 (0) | 69.6 / 73.7 / 77.8 | 0 (0) | 0 / 0 | 0 / 1638 | 6 | 0.0% | 146 / 25 |
| rtt-50 | 50 ms RTT | 329 | 60 | 6.2 (0) | 88.1 / 90.1 / 151.6 | 0 (0) | 0 / 0 | 0 / 1631 | 6 | 0.0% | 147 / 25 |
| rtt-100 | 100 ms RTT | 638 | 60 | 6.1 (0) | 131.1 / 135.2 / 262.1 | 0 (0) | 13 / 0 | 1 / 1617 | 7 | 0.0% | 149 / 25 |
| rtt-200 | 200 ms RTT | 1201 | 58 | 6.0 (0) | 184.3 / 225.3 / 385.0 | 3 (344) | 51 / 0 | 22 / 1587 | 9 | 0.0% | 148 / 23 |
| jitter | 50 ms RTT, ±10 ms jitter, in order | 330 | 60 | 6.2 (0) | 104.4 / 106.5 / 184.3 | 1 (141) | 0 / 0 | 0 / 1628 | 6 | 0.0% | 146 / 24 |
| reorder-2 | 50 ms RTT, 2% of packets 25 ms early | 326 | 60 | 6.2 (0) | 81.9 / 86.0 / 151.6 | 0 (0) | 0 / 0 | 0 / 1628 | 6 | 0.0% | 145 / 25 |
| reorder | 50 ms RTT, ±10 ms jitter, every packet on its own | 306 | 59 | 5.8 (1) | 90.1 / 96.3 / 172.0 | 2 (282) | 18 / 0 | 9 / 1634 | 9 | 0.0% | 151 / 24 |
| loss-1 | 50 ms RTT, 1% loss | 332 | 60 | 6.2 (0) | 120.8 / 122.9 / 176.1 | 1 (110) | 0 / 0 | 0 / 1623 | 6 | 3.2% | 146 / 25 |
| loss-2 | 50 ms RTT, 2% loss | 329 | 59 | 6.1 (0) | 127.0 / 131.1 / 184.3 | 1 (101) | 0 / 0 | 0 / 1623 | 6 | 5.6% | 146 / 25 |
| loss-5 | 50 ms RTT, 5% loss | 338 | 60 | 6.2 (0) | 139.3 / 143.4 / 176.1 | 0 (0) | 0 / 0 | 0 / 1619 | 6 | 10.3% | 148 / 25 |
| loss-both | 50 ms RTT, 2% loss both ways | 361 | 60 | 6.2 (0) | 120.8 / 124.9 / 167.9 | 0 (0) | 0 / 0 | 0 / 1630 | 6 | 5.3% | 149 / 25 |
| burst | 50 ms RTT, bursts of loss (~4%) | 329 | 60 | 6.2 (0) | 143.4 / 143.4 / 163.8 | 0 (0) | 1 / 0 | 0 / 1629 | 6 | 6.8% | 147 / 25 |
| cap-20 | 50 ms RTT, 20 Mbit/s, 100 ms buffer | 334 | 60 | 6.2 (0) | 88.1 / 90.1 / 155.6 | 0 (0) | 0 / 0 | 0 / 1619 | 6 | 0.0% | 149 / 25 |
| cap-6 | 50 ms RTT, 6 Mbit/s (below the stream) | 335 | 59 | 4.1 (2) | 108.5 / 147.5 / 196.6 | 0 (0) | 8 / 0 | 1 / 1612 | 13 | 0.0% | 146 / 22 |
| cap-drop | 50 ms RTT, 20 Mbit/s, 4 Mbit/s from 5 s to 10 s | 331 | 55 | 3.3 (5) | 208.9 / 225.3 / 245.8 | 5 (1135) | 38 / 9 | 29 / 1627 | 19 | 0.6% | 150 / 20 |

| scenario | first sound, ms | latency p50 / p95 / p99 / max, ms | jitter buffer delay / target, ms | lost / late | FEC / concealed, ms | breaks | kbit/s | pictures apart p50 / p95 / max (mean), ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| local | 35 | 56.3 / 57.3 / 58.4 / 59.5 | 25 / 22 | 0 / 0 | 0 / 0 | 0 | 33.3 | 0.5 / 4.0 / 8.1 (0.7) |
| rtt-20 | 107 | 69.6 / 69.6 / 71.7 / 78.6 | 28 / 24 | 0 / 0 | 0 / 0 | 0 | 33.3 | 0.6 / 3.6 / 34.6 (0.8) |
| rtt-50 | 206 | 88.1 / 88.1 / 88.1 / 90.7 | 32 / 24 | 1 / 1 | 0 / 20 | 0 | 33.3 | 0.6 / 3.3 / 126.5 (1.8) |
| rtt-100 | 346 | 129.0 / 131.1 / 131.1 / 132.8 | 48 / 24 | 3 / 3 | 0 / 80 | 0 | 33.3 | 0.6 / 3.3 / 243.2 (3.1) |
| rtt-200 | 658 | 184.3 / 221.2 / 222.3 / 222.3 | 51 / 24 | 7 / 4 | 0 / 200 | 1 | 33.3 | 0.6 / 3.8 / 469.0 (4.6) |
| jitter | 201 | 104.4 / 104.4 / 106.5 / 172.4 | 65 / 60 | 2 / 2 | 0 / 60 | 0 | 33.3 | 0.7 / 4.0 / 179.0 (2.6) |
| reorder-2 | 198 | 81.9 / 84.0 / 84.0 / 84.2 | 52 / 48 | 1 / 1 | 0 / 20 | 0 | 33.3 | 0.7 / 4.2 / 133.8 (2.2) |
| reorder | 188 | 90.1 / 92.2 / 92.2 / 93.4 | 60 / 66 | 2 / 2 | 0 / 60 | 0 | 33.3 | 0.6 / 5.5 / 142.8 (2.2) |
| loss-1 | 199 | 118.8 / 120.8 / 127.0 / 156.9 | 64 / 58 | 8 / 8 | 60 / 120 | 0 | 33.3 | 0.9 / 10.0 / 140.7 (3.5) |
| loss-2 | 200 | 127.0 / 127.0 / 135.2 / 192.9 | 70 / 58 | 10 / 10 | 100 / 100 | 0 | 33.3 | 0.6 / 6.5 / 141.9 (4.5) |
| loss-5 | 198 | 139.3 / 139.3 / 139.3 / 148.4 | 81 / 78 | 3 / 3 | 60 / 80 | 0 | 33.3 | 0.6 / 3.8 / 132.7 (1.8) |
| loss-both | 200 | 120.8 / 120.8 / 122.9 / 123.7 | 64 / 59 | 8 / 8 | 160 / 60 | 0 | 33.3 | 0.8 / 7.2 / 158.6 (4.4) |
| burst | 199 | 139.3 / 140.8 / 143.4 / 166.2 | 84 / 79 | 5 / 5 | 40 / 120 | 0 | 33.3 | 0.6 / 4.2 / 135.7 (2.3) |
| cap-20 | 206 | 88.1 / 88.1 / 90.1 / 98.9 | 32 / 28 | 0 / 0 | 0 / 0 | 0 | 33.3 | 0.6 / 3.0 / 136.0 (1.9) |
| cap-6 | 200 | 108.5 / 110.6 / 110.6 / 120.3 | 52 / 50 | 2 / 2 | 0 / 80 | 0 | 33.3 | 0.7 / 33.8 / 155.4 (5.1) |
| cap-drop | 198 | 208.9 / 225.3 / 225.3 / 232.0 | 146 / 122 | 13 / 12 | 20 / 380 | 1 | 33.3 | 0.6 / 12.0 / 137.5 (3.8) |

Against stage 1's rows: the pictures' latency at the median, before → now, ms.

| local | rtt-20 | rtt-50 | rtt-100 | rtt-200 | jitter | reorder-2 | reorder | loss-1 | loss-2 | loss-5 | loss-both | burst | cap-20 | cap-6 | cap-drop |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 30 → 57 | 45 → 70 | 66 → 88 | 90 → 131 | 143 → 184 | 60 → 104 | 49 → 82 | 68 → 90 | 111 → 121 | 109 → 127 | 156 → 139 | 115 → 121 | 123 → 143 | 70 → 88 | 92 → 109 | 80 → 209 |

- **Lip sync: met everywhere.** Pictures and sound play within 1 ms of each other at the median,
  within 10 ms at p95 on every path but the narrowed ones (`cap-6` 34 ms, `cap-drop` 12 ms).
  The maximum, 100–470 ms, is pictures that are late on their own: the joining keyframe on long
  round trips, a stall.
- **The sound holds.** No break under any loss (1–5%, both ways, bursts). FEC and concealment
  cover what was lost, 100–200 ms of 15 s. One break each on `rtt-200` (a 200 ms lifetime that
  repair cannot meet) and `cap-drop` (the path at 4 Mbit/s under a 6 Mbit/s stream). Next to the
  keyframes on `cap-6` (13 a run) there is none.
- **The pictures got slower, and the sound is why.** They wait for it now, and its latency has
  three parts the pictures never had: a frame to fill and one for FEC (40 ms on any path), and a
  jitter buffer that stays wherever losses pushed it. With no silence to cut, the delay comes
  back only once it is more than a frame and a half over target for a second (`rtt-100`: 48 ms
  for a target of 24, so +41 ms to the pictures). `cap-drop` shows it worst: the 5 s at 4 Mbit/s
  push the sound's target to 122 ms, it stays there, and the pictures follow, 80 → 209 ms at the
  median. Video-only, this row was 80 ms.

What it leaves for the stages that take the latency profiles up (5 and 6): the frame for FEC only
on lossy paths, and the sound's delay brought down after a spike without waiting for silence
(a tighter drop threshold, or time-stretching). Until then, a viewer who wants the pictures soonest
plays them out of step (`--no-sync`): stage 1's latencies.

## Against SRT and librist: a first look (3.2 a)

The harness's first run, before any change for it: ratcast 6541c39 + the probe, librats 2141d8b,
SRT 1.5.7, librist 0.2.20 (main profile). `bench/netem.py --feed --duration 8 local loss-2`:
the probe at 8 Mbit/s, held 200 ms by each transport; one run each, so a direction rather than
a result.

| scenario | transport | first packet, ms | latency p50 / p99 / max, ms | lost, % | glitches | longest gap, ms | link → / ←, Mbit/s | CPU send / recv, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| local | ratcast | 205 | 200.7 / 208.9 / 242.0 | 0.000 | 0 | 43 | 8.85 / 0.37 | 6.0 / 7.1 |
| local | srt | 195 | 208.9 / 213.0 / 217.5 | 0.000 | 0 | 18 | 8.39 / 0.06 | 2.3 / 2.7 |
| local | rist | 195 | 204.8 / 213.0 / 220.9 | 0.000 | 0 | 26 | 8.45 / 0.02 | 2.9 / 2.2 |
| loss-2 | ratcast | 357 | 229.4 / 233.5 / 271.6 | 0.910 | 33 | 47 | 8.61 / 0.34 | 6.0 / 8.0 |
| loss-2 | srt | 549 | 233.5 / 241.7 / 245.1 | 0.000 | 0 | 21 | 8.03 / 0.05 | 2.3 / 2.7 |
| loss-2 | rist | 249 | 229.4 / 233.5 / 240.7 | 0.000 | 0 | 20 | 8.39 / 0.04 | 2.6 / 2.9 |

- **Latency is the buffer, for all three:** 200 ms past the path, give or take the meter's
  4 ms buckets. ratcast keeps it tightest (p99 − p50 = 4–8 ms), and its maximum is the
  highest (242 / 272 ms): a frame held up by the machine, not the path, since the longest gap
  is too (43–47 ms against 18–26).
- **ratcast loses 0.9% on `loss-2`, the others nothing — all of it on joining.** The viewer's
  first second lost 106 frames (636 packets); every second after, none. They expired in the
  sender's queue (`live_expired=106` of 11932): the connection starts slower than the feed and
  cannot catch up under loss while their lifetime runs. SRT and librist send at the input's
  rate from the first packet. Repair itself holds: no loss after that second.
- **More back, a little more forth.** ratcast's viewer sends 0.35 Mbit/s back, against
  0.02–0.06 for the other two; what it is (acknowledgements, path probes) is still to be
  looked at. Forward, it puts 8.6–8.9 Mbit/s on the link for an 8 Mbit/s feed (+8–11%), SRT
  and librist 8.0–8.5 (+0–6%): likely librats' headers and encryption on 1128-byte frames,
  against their 1316-byte payloads.
- **2–3× the CPU** at each end (6–8% against 2–3% of a core). Part of it is encryption,
  which SRT and librist are built without here.
- **Joining**: on `local` all three take about the buffer (195–205 ms) to the first packet out.
  On `loss-2` SRT took 549 ms, ratcast 357, librist 249 — one run each, the handshake's
  packets lost or not.

### Why ratcast loses on joining

Not the loss: the round trip. `bench/netem.py --feed --transport ratcast --duration 5`, and the
same start driven through librats' `UdpConnection` in virtual time (a responder sending the feed
as `DataPublisher` does — 752- and 564-byte live messages at 8 Mbit/s, 200 ms lifetime — from
the moment the connection is up):

| scenario | expired, virtual time | expired, netem | lost at the meter, % |
|---|---:|---:|---:|
| rtt-20 | 0 | 0 | 0 |
| rtt-50 | 101 | 99 | 1.42 |
| loss-2 (also 50 ms) | 101–112 (seeds) | 106 | 0.91 |
| rtt-100 | 456 | 456 | 6.75 |

1. A fresh connection starts in BBR's Startup with a ten-packet window: on a 50 ms round trip
   it paces at 2.2 Mbit/s once the first round trip is measured, about doubles each round,
   and reaches the feed's 8 Mbit/s after 200–250 ms (100 ms round trip: 1.1 Mbit/s, twice as
   long).
2. A TS feed comes at its full rate from the first packet and cannot be slowed. ratcast's video
   never showed this: its rate control starts at 2 Mbit/s.
3. `LiveSender::shed` (librats, `transport/udp_live.cpp`) gives up a message not yet started
   when `now + one_way + rate.time_for(ahead + owed) > deadline`, `rate` the pacing rate *now*.
   In Startup that is 3–10× below what it is a few round trips later, so messages are judged
   hopeless that would have made it: in virtual time every one of the 101 goes between 75 and
   150 ms, while the earliest deadline is 200 ms — shed, not expired — and by what was
   delivered when, the backlog would have drained in time without them.

SRT's live mode and librist have no congestion control to start up: they send at the input's
rate from the first packet. This is librats' L2 (b), put off with stage 1 for video on long paths
(`docs/PLAN.md`); a full-rate feed meets it from a 50 ms round trip. Two ways out, both in
librats: shed by the pacing rate only once Startup is over (before it, by deadline alone), or let
the application say the rate a stream comes at, for the connection to start at.

### Where the rest goes: overhead, feedback, CPU

Measured the same three ways: librats in virtual time (3 s of steady state, the feed as
`DataPublisher` sends it), the tools on one host with no shaping (8 s, per process: `/proc` for
user and system time, `strace -c` for system calls), and `callgrind` at 2 Mbit/s for where user
time goes (perf is not allowed in this VM).

**Every datagram of the feed goes as two packets.** A UDP feed comes in datagrams of seven TS
packets (1316 bytes); `DataPublisher` sends each read at once, in frames as even as they come —
four packets and three (752 and 564 bytes) — and librats carries each in a packet of its own (a
datagram holds 1220 bytes, so the two never share one). 8 Mbit/s is 1520 packets a second
where SRT sends 760.

| | packets/s | bytes a packet beyond the payload | on the link |
|---|---:|---:|---:|
| ratcast → | 1520 | 66 (IP/UDP 28, header and live frame ~22, AEAD 16) | +10.0% |
| ratcast ← | 760 | 62: an ACK, every second packet | 0.38 Mbit/s |
| SRT → | 760 | 44 (IP/UDP 28, SRT 16) | +4.9% (netem) |
| SRT ← | ~100 | an ACK every 10 ms | 0.05 Mbit/s |

Half the packets would make it +5%: most of the gap is the packet count, two half-full packets
a datagram; the rest against SRT is a heavier packet (the connection id, the live frame's
header, the AEAD tag). The feedback is RFC 9000's ACK every second packet, at twice the
packets.

**The CPU is in the kernel, not in the cipher.** On one host at 8 Mbit/s:

| | user, % | system, % |
|---|---:|---:|
| ratcast-send | 1.9 | 5.6 |
| ratcast-recv | 1.5 | 6.6 |
| srt-live-transmit, sending | 0.6 | 2.2 |
| srt-live-transmit, receiving | 1.1 | 2.0 |

Three quarters of ratcast's CPU is system time: about 9500 system calls a second at the viewer
and 6000 at the sender, against ~1000 packets a second of feed. At the viewer:
- `futex` ~4000/s: `DataPlayer::push()` wakes the writer for every frame though it is due 200
  ms later, and the writer wakes again at each frame's time;
- `sendto` ~1500/s: `DataPlayer` writes each frame as it falls due, so the feed goes out in
  datagrams of four and three packets, twice as many as came in;
- `recvmmsg` ~1500/s and `sendmmsg` ~700/s: librats' packets in and its ACKs out.
At the sender the same packets and ACKs the other way, and the input's `poll` and two
`recvfrom` a datagram (the second finds nothing). The cipher is librats' portable ChaCha20-Poly1305,
~15 instructions a byte — a third of user time, about 0.5% of a core: worth a faster one some
day, not the gap to SRT.

One `rtt-20` run had a 206 ms gap and a p99 of 369 ms with nothing expired; three more had
neither (p99 217 ms, gaps of 10–11 ms): the VM, not the transport.

Joining takes ratcast ~100 ms more than librist (`loss-2`: 357 ms against 249 to the first packet
out): the handshake and the stream's announcement and subscription, a round trip each, before
the first frame goes — L2's put-off parts and 1.2's, not this stage's.

### Joining, fixed (3.2 (b) 1, librats L2 (b))

What fixed it, and what did not (`docs/PLAN.md`, L2 (b)): not shedding in Startup saved the 50 ms
path and lost more on longer ones — a feed faster than Startup's climb builds a backlog that
shedding is what clears. Starting at the feed's own rate does: until the controller has a
verdict on the path it paces no slower than live data is offered, through a window that lets
that out. librats 2141d8b + the change, uncommitted.

Virtual time (a responder sending 8 Mbit/s of live messages from the moment the connection is up,
200 ms lifetime, 2% loss; three seeds):

| round trip | expired before | after |
|---|---:|---:|
| 50 ms | 1.5% | 0 |
| 100 ms | 6.4% | 0.05–0.17% |
| 200 ms | 20% | 1.1–3.9% (no time to repair in 200 ms) |

With a bottleneck and no loss: 10 or 20 Mbit/s, 50 or 100 ms, 1.3–5.3% → 0; 6 Mbit/s, below the
feed, 28–32% before and after (8 into 6 does not go).

netem, `bench/netem.py --feed --transport ratcast --duration 6`, one run each:

| scenario | lost before, % | after, % | expired after |
|---|---:|---:|---:|
| local | 0 | 0 | 0 |
| rtt-50 | 1.42 | 0 | 0 |
| rtt-100 | 6.75 | 0 | 0 |
| loss-2 | 0.91 | 0 | 0 |
| burst | – | 0.02 | 2 |
| cap-6 | – | 33.8 | 3167 (the feed is above the path) |

`bench_path --realtime --seeds=8`, before → after, the rows that moved by more than noise:

| row | before | after |
|---|---:|---:|
| fixed-rate video by class, far path (100 Mbit, 200 ms), % late | 2.6 / 3.6 / 4.3 / 6.7 / 3.5 | 0.5 / 0.9 / 0.8 / 1.3 / 2.3 |
| 4 of 10 Mbit, its "200 ms, 1%" row, % late | 3.7 | 1.0 |
| 4 of 10 Mbit + a bulk flow on the path, % late | 22.0 | 16.2 |
| deep buffer + a bulk flow, % late | 30.0 | 24.5 |
| seven peers joining 3 s apart on 20 Mbit, % late | 4.8 | 3.4 |
| sized to the estimate, 1 Gbit at 200 ms: estimate, Mbit/s | 3.3 | 4.2 |
| seven peers starting together on 20 Mbit, % late | 3.3 | 3.6 |
| 1.5 of 2 Mbit, 40 ms, % late (six columns) | 5.0 / 4.6 / 4.1 / 5.6 / 5.8 / 5.1 | 5.3 / 3.9 / 4.0 / 5.7 / 6.2 / 5.5 |

A floor a quarter over the feed drained the first round trip's backlog sooner (100 ms: 0.07%,
200 ms: 1.8%) but made the seven peers starting together 4.3% late; the floor is the feed's own
rate.

Video, `bench/netem.py --audio`, against stage 2's medians: the joining keyframe comes sooner
too. `rtt-100` (one run of 10 s) shows its first picture in 364 ms (638), 111 ms to the picture
at the median (131); `cap-drop` 216 ms (331) and 82 (209); `local` as it was (55 ms, 54).
`cap-6` over three runs of 15 s: first picture 230 ms (335), 102 / 135 / 172 ms at p50 / p95 /
p99 (108 / 148 / 197) — and a stall a run (none in stage 2's medians): one on joining, two as
rate control climbs into the 6 Mbit/s, to watch in the full table.

Reviewed after the commit (librats 33f5e88), two fixes and a cost:
- Reno reported its bandwidth as one `cwnd()` per round trip, and `cwnd()` holds the floor: a
  source sizing itself to `PathStats::bandwidth_bps` was told twice its own rate, raised it, and
  the floor with it, until a loss. `bench_path --cc=reno`, sized to the estimate, four peers
  joining 5 s apart: 5.0% of frames late before the change, 8.7% with it, 6.2% now that Reno
  reports its own window (`bandwidth()`). BBR reports its model and was never affected.
- Reno paced the floor's window at slow start's gain — two and more times the feed; now at the
  feed's rate, as BBR does.
- `OfferedRate` kept its points on every connection: now nothing until something live is
  pushed, and given back once the feed stops. What is left is 80 bytes a connection
  (`bench_path` memory: an idle pair 18 → 19 KiB, rounded); every other section without live
  data is bit for bit the same, and BBR's realtime section the same as the commit's.

### Full frames, the writer woken less (3.2 (b) 2, 3)

With librats' L2 (b) in ("Joining, fixed"), `DataPublisher` sends full frames — six packets, what is left of a read waiting for the next,
or for the input to be quiet for 5 ms (`TsInput::start`'s `quiet`); `DataPlayer` is woken only
by a frame due sooner than the one it waits for, and writes the frames due together at once.
`bench/netem.py --feed --duration 8 local loss-2 rtt-100`, one run each:

| scenario | transport | first packet, ms | latency p50 / p99 / max, ms | lost, % | glitches | longest gap, ms | link → / ←, Mbit/s | CPU send / recv, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| local | ratcast | 211 | 200.7 / 208.9 / 212.7 | 0.000 | 0 | 17 | 8.57 / 0.21 | 5.9 / 6.3 |
| local | srt | 198 | 208.9 / 213.0 / 218.5 | 0.000 | 0 | 19 | 8.40 / 0.06 | 2.5 / 2.7 |
| local | rist | 197 | 204.8 / 208.9 / 216.1 | 0.000 | 0 | 22 | 8.45 / 0.02 | 3.2 / 2.4 |
| rtt-100 | ratcast | 513 | 254.0 / 258.0 / 282.6 | 0.000 | 0 | 34 | 8.26 / 0.20 | 6.0 / 7.1 |
| rtt-100 | srt | 397 | 258.0 / 268.6 / 268.6 | 0.000 | 0 | 16 | 8.19 / 0.05 | 2.5 / 3.1 |
| rtt-100 | rist | 300 | 254.0 / 258.0 / 269.7 | 0.000 | 0 | 23 | 8.34 / 0.02 | 2.9 / 3.1 |
| loss-2 | ratcast | 358 | 229.4 / 229.4 / 237.7 | 0.000 | 0 | 15 | 8.42 / 0.22 | 6.0 / 7.6 |
| loss-2 | srt | 297 | 233.5 / 241.0 / 241.0 | 0.000 | 0 | 16 | 8.28 / 0.05 | 2.3 / 2.9 |
| loss-2 | rist | 248 | 229.4 / 233.5 / 238.4 | 0.000 | 0 | 13 | 8.39 / 0.04 | 2.9 / 3.1 |

- **Nothing lost on joining** still, and ratcast keeps the tightest p99 of the three.
- **Packets: 1520 → 886 a second** (5.99 TS packets a frame): the link's overhead +7–10% →
  +3–7%, the feedback 0.37 → 0.21 Mbit/s (still an ACK every second packet: L13).
- **System calls, one host, 8 Mbit/s:** the viewer's ~9500 a second → ~5650 (`futex` 4000 →
  1300, `sendto` 1500 → 865), srt-live-transmit's ~3900. CPU, user + system: sender 7.5 → 6.2%,
  viewer 8.1 → 6.2%; SRT 2.8 / 3.1%. The netem runs, a busier VM, show less of it (6.0 / 7.1 →
  6.0 / 7.6 on `loss-2`, within a run's noise).

What is left of the CPU, by thread at the viewer: the writer 3.5% (a timer wake-up and a
`sendto` per frame — the feed's own pace; fewer would mean writing up to a millisecond or two
early or late) and librats' reactor 2.8%. The reactor's share is per turn, not per byte: every
turn empties its wake-up socket with a `recv` whether or not it was signalled (~830 a second,
all of them failing), every `recvmmsg` that found datagrams is followed by one that finds none,
and at the sender every frame handed to it wakes it with a `send` to that socket (~650 a
second) though one is already pending. That is librats work (L15). The feed reaches the
sender's reactor and leaves the viewer's through a thread of its own, one hand-over more at
each end than srt-live-transmit makes.

### Fewer ACKs, tried (librats L13)

The feedback after full frames: an ACK every second packet, ~440 a second for 8 Mbit/s — 0.22
Mbit/s back, 2.7% of the feed (SRT: an ACK every 10 ms, 0.05). Tried in librats as QUIC's
ACK_FREQUENCY, put off (`docs/PLAN.md`, L13): the sender asks the receiver to acknowledge
packets of live data every max(2, window / 4), a packet out of order and the third past a gap
still at once; BBR is told, so the batching it asked for is not read as a path that holds
acknowledgements back. Measured in librats' own suites, in virtual time:

- **In Startup**, with every 10: Startup's end on a burst that queues needs six groups of
  acknowledgements rising in a row, and a video frame was one or two of them — five BBR tests
  failed (Startup never ended, a call padded to verify a queue that was not there).
- **Kept out of Startup** and BBR's probes, a constant-rate feed on a path it never fills stays
  in Startup for good (a TS feed of 8 Mbit/s on an unlimited path, 6 s: Startup throughout), so
  it gains nothing: 443 ACKs a second before and after.
- **Between probes**, at most four to an ACK, `bench_path`'s realtime rows: beside a bulk flow
  12.2 → 20.5% of the frames late; the link dropping to 3 Mbit/s, the longest freeze 638 → 2311
  ms; a few rows a little better (3% in 100 ms fades 3.7 → 2.7% late).

The acknowledgements are BBR's sensor — it reads the queue off them one by one — so fewer of
them cost live video its controller's reactions. What would keep both: receive timestamps, the
receiver reporting when each packet arrived, in batches, as WebRTC's transport-wide feedback
does.

## Against SRT and librist, measured whole (3.2)

ratcast f1f8ffa (3.2 (b) 1–3 in, L13 not), librats 3669d2c, SRT 1.5.7, librist 0.2.20 (main
profile), each built as `bench/peers.sh` builds it — SRT and librist without encryption, ratcast
always encrypted. `bench/netem.py --feed --repeat 3`: the stamped probe at 8 Mbit/s, held 200 ms
by each transport, 15 s a run, medians of three.

| scenario | transport | first packet, ms | latency p50 / p99 / max, ms | lost, % | glitches | longest gap, ms | link → / ←, Mbit/s | CPU send / recv, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| local | ratcast | 206 | 200.7 / 208.9 / 218.4 | 0.000 | 0 | 21 | 8.58 / 0.22 | 5.5 / 6.3 |
| local | srt | 196 | 208.9 / 217.1 / 223.5 | 0.000 | 0 | 23 | 8.40 / 0.06 | 2.6 / 2.8 |
| local | rist | 198 | 204.8 / 213.0 / 226.2 | 0.000 | 0 | 37 | 8.45 / 0.02 | 3.1 / 2.4 |
| rtt-20 | ratcast | 270 | 213.0 / 217.1 / 226.8 | 0.000 | 0 | 18 | 8.55 / 0.21 | 5.2 / 6.6 |
| rtt-20 | srt | 248 | 217.1 / 225.3 / 238.4 | 0.000 | 0 | 28 | 8.38 / 0.06 | 2.5 / 2.8 |
| rtt-20 | rist | 220 | 213.0 / 225.3 / 250.2 | 0.000 | 0 | 40 | 8.43 / 0.02 | 2.7 / 2.9 |
| rtt-50 | ratcast | 358 | 229.4 / 233.5 / 251.2 | 0.000 | 0 | 28 | 8.50 / 0.20 | 5.4 / 6.8 |
| rtt-50 | srt | 299 | 233.5 / 241.7 / 251.4 | 0.000 | 0 | 23 | 8.33 / 0.05 | 2.4 / 2.8 |
| rtt-50 | rist | 251 | 229.4 / 237.6 / 251.5 | 0.000 | 0 | 25 | 8.42 / 0.02 | 2.7 / 2.9 |
| rtt-100 | ratcast | 511 | 254.0 / 258.0 / 270.1 | 0.000 | 0 | 24 | 8.43 / 0.21 | 5.3 / 6.8 |
| rtt-100 | srt | 399 | 258.0 / 270.3 / 300.1 | 0.000 | 0 | 48 | 8.27 / 0.05 | 2.5 / 2.7 |
| rtt-100 | rist | 297 | 254.0 / 270.3 / 275.6 | 0.000 | 0 | 27 | 8.39 / 0.02 | 2.7 / 2.9 |
| rtt-200 | ratcast | 811 | 303.1 / 311.3 / 319.2 | 0.008 | 1 | 20 | 8.25 / 0.21 | 5.2 / 6.5 |
| rtt-200 | srt | 600 | 311.3 / 319.5 / 326.3 | 0.000 | 0 | 26 | 8.16 / 0.05 | 2.4 / 2.7 |
| rtt-200 | rist | 402 | 303.1 / 311.3 / 321.0 | 0.000 | 0 | 21 | 8.33 / 0.02 | 2.6 / 2.8 |
| jitter | ratcast | 361 | 221.2 / 225.3 / 229.7 | 0.000 | 0 | 13 | 8.69 / 0.08 | 4.9 / 6.0 |
| jitter | srt | 313 | 221.2 / 229.4 / 242.9 | 0.000 | 0 | 26 | 8.31 / 0.05 | 2.2 / 2.7 |
| jitter | rist | 270 | 241.7 / 245.8 / 257.6 | 0.000 | 0 | 26 | 8.40 / 0.02 | 2.7 / 2.5 |
| reorder-2 | ratcast | 336 | 200.7 / 208.9 / 227.0 | 0.000 | 0 | 22 | 8.68 / 0.26 | 5.2 / 6.6 |
| reorder-2 | srt | 301 | 233.5 / 241.7 / 250.1 | 0.000 | 0 | 21 | 10.84 / 0.06 | 2.6 / 2.9 |
| reorder-2 | rist | 255 | 229.4 / 237.6 / 247.1 | 0.000 | 0 | 25 | 8.44 / 0.02 | 2.7 / 2.8 |
| reorder | ratcast | 327 | 200.7 / 204.8 / 212.3 | 0.354 | 4 | 33 | 8.62 / 0.31 | 5.2 / 6.2 |
| reorder | srt | 304 | 237.6 / 245.8 / 267.3 | 0.000 | 0 | 46 | 15.56 / 0.13 | 2.9 / 3.6 |
| reorder | rist | 240 | 225.3 / 229.4 / 239.7 | 0.000 | 0 | 30 | 8.83 / 0.04 | 2.7 / 2.9 |
| loss-1 | ratcast | 356 | 229.4 / 229.4 / 237.6 | 0.000 | 0 | 15 | 8.51 / 0.21 | 5.2 / 6.9 |
| loss-1 | srt | 300 | 233.5 / 241.7 / 248.9 | 0.000 | 0 | 19 | 8.33 / 0.05 | 2.2 / 2.7 |
| loss-1 | rist | 250 | 229.4 / 241.7 / 265.0 | 0.000 | 0 | 41 | 8.42 / 0.03 | 2.7 / 2.9 |
| loss-2 | ratcast | 361 | 229.4 / 233.5 / 246.8 | 0.000 | 0 | 24 | 8.51 / 0.22 | 5.2 / 6.7 |
| loss-2 | srt | 299 | 233.5 / 241.7 / 249.8 | 0.000 | 0 | 30 | 8.34 / 0.05 | 2.3 / 2.6 |
| loss-2 | rist | 249 | 229.4 / 241.7 / 256.1 | 0.000 | 0 | 30 | 8.42 / 0.03 | 2.7 / 2.8 |
| loss-5 | ratcast | 360 | 229.4 / 233.5 / 246.4 | 0.000 | 0 | 23 | 8.50 / 0.22 | 5.3 / 6.7 |
| loss-5 | srt | 305 | 233.5 / 241.7 / 257.8 | 0.000 | 0 | 35 | 8.39 / 0.06 | 2.4 / 2.7 |
| loss-5 | rist | 250 | 229.4 / 237.6 / 250.0 | 0.009 | 1 | 27 | 8.42 / 0.05 | 2.7 / 2.9 |
| loss-both | ratcast | 358 | 229.4 / 233.5 / 244.7 | 0.000 | 0 | 20 | 8.51 / 0.21 | 5.2 / 6.7 |
| loss-both | srt | 299 | 233.5 / 241.7 / 247.9 | 0.000 | 0 | 22 | 8.34 / 0.05 | 2.2 / 2.5 |
| loss-both | rist | 248 | 229.4 / 237.6 / 247.9 | 0.000 | 0 | 24 | 8.42 / 0.03 | 2.7 / 2.9 |
| burst | ratcast | 358 | 229.4 / 233.5 / 251.3 | 0.000 | 0 | 31 | 8.53 / 0.21 | 5.3 / 6.9 |
| burst | srt | 299 | 233.5 / 241.7 / 250.5 | 0.000 | 0 | 24 | 8.40 / 0.05 | 2.3 / 2.6 |
| burst | rist | 249 | 229.4 / 241.7 / 259.1 | 0.009 | 1 | 37 | 8.43 / 0.03 | 2.7 / 2.9 |
| cap-20 | ratcast | 363 | 229.4 / 229.4 / 238.7 | 0.000 | 0 | 15 | 8.51 / 0.21 | 5.2 / 7.0 |
| cap-20 | srt | 303 | 233.5 / 241.7 / 254.1 | 0.000 | 0 | 27 | 8.33 / 0.05 | 2.5 / 3.1 |
| cap-20 | rist | 254 | 229.4 / 237.6 / 249.8 | 0.000 | 0 | 25 | 8.42 / 0.02 | 2.6 / 3.0 |
| cap-6 | ratcast | 361 | 229.4 / 368.6 / 375.2 | 32.245 | 2649 | 135 | 5.77 / 0.16 | 6.1 / 5.8 |
| cap-6 | srt | 300 | 233.5 / 241.7 / 242.7 | 51.051 | 1334 | 28 | 5.96 / 0.11 | 2.7 / 2.9 |
| cap-6 | rist | 254 | 344.1 / 344.1 / 352.2 | 28.734 | 2785 | 109 | 5.98 / 0.14 | 2.7 / 2.8 |
| cap-drop | ratcast | 358 | 229.4 / 278.5 / 319.2 | 19.860 | 1093 | 73 | 6.89 / 0.18 | 5.5 / 5.6 |
| cap-drop | srt | 298 | 233.5 / 241.7 / 250.8 | 23.011 | 410 | 31 | 6.97 / 0.07 | 2.6 / 3.0 |
| cap-drop | rist | 251 | 229.4 / 237.6 / 252.4 | 17.796 | 1297 | 28 | 6.97 / 0.07 | 2.7 / 2.8 |

- **Nothing lost where the path has room**, by any of the three: every delay, loss (1–5%, both
  ways, bursts) and jitter scenario comes through whole after repair. The exceptions are
  small: ratcast 0.35% on `reorder` (below) and 0.008% on `rtt-200` (1–3 frames shed on joining
  in two runs of three, what is left of L2 (b) at a 200 ms round trip); librist 0.009% on
  `loss-5` and `burst`.
- **The same latency, ratcast the steadiest.** All three play the feed at the buffer past the
  path's one-way delay (`loss-2`: 229 / 234 / 229 ms at the median). ratcast's p99 is 4–8 ms over
  its median, SRT's and librist's 8–16. On `jitter` librist plays 20 ms later than the other two.
  On `reorder` and `reorder-2` ratcast plays 25–35 ms sooner: it counts the buffer from the
  fastest transit it has seen, the other two from a typical one.
- **Joining: ratcast is slowest**, about two round trips more than librist — the handshake, then
  the stream's announcement and subscription before the first frame. To the first packet out:
  `rtt-50` 358 / 299 / 251 ms (ratcast / SRT / librist), `rtt-100` 511 / 399 / 297, `rtt-200`
  811 / 600 / 402.
- **On the link:** ratcast +3–9% over the feed, SRT +2–5% and librist +4–6% — except where
  packets are reordered: SRT sends 10.8 Mbit/s on `reorder-2` and 15.6 on `reorder`, nearly the
  feed twice over (it takes reordering for loss and sends again), librist 8.8, ratcast 8.6–8.7.
  Back: ratcast 0.2–0.3 Mbit/s (an ACK every second packet; L13 put off), SRT 0.05, librist
  0.02–0.05.
- **CPU: ratcast twice theirs**, 5–6% at the sender and 6–7% at the viewer against 2–3.5%: system
  calls per reactor turn in librats (L15) and the hand-over to and from a thread of the feed's
  own at each end ("Full frames, the writer woken less").
- **Over capacity** (`cap-6`: 6 Mbit/s for an 8 Mbit/s feed, a quarter must go; `cap-drop`: 4
  Mbit/s for 5 s, a sixth of the run's feed) no transport of a fixed-rate feed can help, and
  they differ in how they fail. librist loses the least (28.7 / 17.8%), ratcast a little more
  (32.2 / 19.9%), SRT the most (51.1 / 23.0%) but holds its latency, dropping what is late at the
  receiver; ratcast and librist play later instead (ratcast's p99 369 ms on `cap-6`, librist's
  median 344 ms), the backlog lifting the transit they count from. A feed that can be made
  smaller is what ratcast's own media has rate control for.

### Why ratcast loses on `reorder`: ProbeRTT

Every packet on its own delay, N(25 ms, 10 ms) each way, as netem's `reorder`: each run loses
one burst, 36–45 frames shed by the sender at once 10–14 s in. The same feed in librats' virtual
time, over a path that delays each packet the same way, shows what it is (three seeds):

1. BBR's minimum round trip is an outlier — 3 ms, both legs near zero at once — where the
   round trip is ~50 ms.
2. BBR skips ProbeRTT while an app-limited sender sees the minimum again, give or take an
   eighth; with this one it never does, so every 5–8 s it enters ProbeRTT, whose window is half
   a BDP *at that minimum*: 6.4 KB where an 8 Mbit/s feed over 50 ms keeps ~50 KB in flight.
3. For ProbeRTT's 200 ms and a round trip the feed waits behind that window: 32–72 frames shed
   an episode, and once (seed 1) another ~100 just after it, the bandwidth estimate briefly down
   to 1.1 Mbit/s.

Paths that delay every packet independently, by tens of milliseconds, are rare (netem's
`jitter`, where packets keep their order, loses nothing), so this is an *improves*:
librats L16, a ProbeRTT that does not starve a live flow (its window no smaller than the live
floor, as Startup's is since L2 (b); or skipped while samples stay within the path's own swing
of the minimum).

## netsim, against netem (4.5)

`ratcast-netsim` emulates a path in user space: a UDP proxy whose two directions follow netem's
rules — loss at random or in Gilbert-Elliott bursts, then a queue of `limit` packets, then the
delay, each packet's own jitter or, with a `rate`, in order at that rate; `reorder` sends a share
at once — and read netem's own words, so a scenario is written once (`bench/netem.py --netsim`).
The same feed, ratcast only, one run each (12 s; the left half is netem, the right netsim):

| scenario | first packet, ms | p50 / p99, ms | lost, % | first packet, ms | p50 / p99, ms | lost, % |
|---|---:|---:|---:|---:|---:|---:|
| rtt-100 | 514 | 254.0 / 258.0 | 0.000 | 512 | 254.0 / 256.0 | 0.000 |
| jitter | 332 | 221.2 / 229.4 | 0.000 | 356 | 221.2 / 225.3 | 0.000 |
| reorder | 340 | 200.7 / 204.8 | 1.034 | 335 | 200.7 / 208.9 | 0.590 |
| burst | 357 | 229.4 / 229.4 | 0.000 | 358 | 229.4 / 233.5 | 0.000 |
| cap-6 | 362 | 229.4 / 385.0 | 32.160 | 362 | 229.4 / 383.1 | 32.116 |
| cap-drop | 359 | 229.4 / 294.9 | 24.947 | 361 | 229.4 / 249.9 | 25.381 |

And all three transports through netsim (8 s, one run), next to netem's full table (15 s,
medians of three): `local` ratcast 203 ms / 200.7 / 204.8 (netem 206 / 200.7 / 208.9), SRT 196 /
208.9 / 213.0 (196 / 208.9 / 217.1), librist 201 / 204.8 / 204.8 (198 / 204.8 / 213.0); `loss-2`
ratcast 356 / 229.4 / 229.4 (361 / 229.4 / 233.5), SRT 298 / 233.5 / 241.7 (299 / 233.5 /
241.7), librist 250 / 229.4 / 241.7, 0.017% lost (249 / 229.4 / 241.7) — first packet, p50, p99.

- **The same paths**, to a few milliseconds and the run-to-run spread, but for one: under a cap
  (below). `reorder` loses on both, a burst a run or two to BBR's ProbeRTT (L16). The netsim
  `rtt-100` row is the median of two runs after a third that stalled 423 ms in its last second,
  the VM's (none of the others did).
- **netsim's timers wake as late as the host's.** On this VM `epoll_wait` and `nanosleep` for
  10 ms come back 0–10 ms late (both, a few hundred times: the vCPU asleep), so a packet's delay
  is never short and sometimes up to 10 ms long; the medians above show nothing of it. netem's
  hrtimers in the kernel do better.
- **Under a cap, netem shapes ratcast with GSO's batches.** librats sends a run of packets to a
  peer as one buffer (UDP GSO), and netem on the sender's own veth takes it as one packet: one
  loss draw for the run, one place in `limit` for all of it. `cap-drop`, 15 s, two runs each:

  | | p99, ms | lost, % |
  |---|---:|---:|
  | netem (the tables above, GSO on) | 287 | 19.9 |
  | netem, GSO off | 256 | 24.1 |
  | netsim (GSO on: the kernel splits the runs before the proxy reads them) | 252 | 19.8 |
  | netsim, GSO off | 256 | 19.6 |

  The extra 30 ms of netem's are its queue of 51 *runs* holding more than 51 packets would; a
  router sees packets, as netsim does. Why netem without GSO loses 4% more than netsim is not
  measured; likely that packets in netem's queue still count against the sender's socket buffer
  (it is on the sender's own egress), which librats takes for a full socket (`ENOBUFS`, `EAGAIN`)
  and stops sending for — backpressure no path beyond the host gives. SRT and librist send no
  GSO, so their rows are not touched. The comparison tables stand (all three crossed the same
  netem, and ratcast's cap rows are the gentler for it), but a cap's latency for ratcast reads
  truer from netsim.


## The GPU path on Windows (4.3 b)

Windows 11, Intel Core Ultra 7 265KF, NVIDIA GeForce RTX 5080, FFmpeg 9.0.1 from vcpkg; MSVC
release build (RelWithDebInfo). `ratcast-send --source screen` on the primary 1920x1080 screen,
av1_nvenc at 20 Mbit/s fixed, with a small window changing colour every refresh so that the
screen sends 60 pictures a second, to `ratcast-recv --headless` on the same host for 15 s;
`--no-gpu` for the path through memory. Medians of three runs.

| | sender CPU, % of a core | encode, ms a picture | pictures received a second | stalls |
|---|---:|---:|---:|---:|
| GPU path | 4 | 3.7 | 58–60 | 0 |
| through memory (`--no-gpu`) | 29 | 2.7 | 58–60 | 0 |

The 25 points are the copy out of the GPU, the conversion on the CPU and NVENC's upload back.
`encode` rises by a millisecond because it now waits for the GPU's conversion, which the path
through memory has done before it starts; with pictures already in GPU memory NVENC takes 0.6–1.8
ms longer at 1440p and 4K than from memory (3.4 → 5.1 ms, 4.1 → 6.0 ms, one run each), less
than the CPU's conversion alone takes at those sizes (`bench_convert` on the dev VM: ~1.8 ms at
1440p, 5.2 ms at 4K).

## Stage 6, measured whole

After 6.1–6.6: ratcast 893ca52, librats 0cd5d06. `bench/netem.py --audio --repeat 3`, the same
run as stage 2's: the pattern, rate-controlled (2 Mbit/s up to 8), the stamped tone beside it,
played in step — now with `ratcast-send` and `ratcast-recv` each a `Call` (6.1), so the tools go
through the app's own call code.

| scenario | path | first picture, ms | fps | Mbit/s (cuts) | latency p50 / p95 / p99, ms | stalls (ms) | lost / dropped | expired / sent | keyframes | repair | CPU send / recv, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| local | veth, no shaping | 72 | 60 | 6.2 (0) | 59.4 / 63.5 / 64.5 | 0 (0) | 0 / 0 | 0 / 1634 | 6 | 0.0% | 147 / 25 |
| rtt-20 | 20 ms RTT | 145 | 60 | 6.2 (0) | 69.6 / 75.8 / 77.8 | 0 (0) | 0 / 0 | 0 / 1626 | 6 | 0.0% | 146 / 25 |
| rtt-50 | 50 ms RTT | 217 | 60 | 6.2 (0) | 81.9 / 84.0 / 88.1 | 0 (0) | 0 / 0 | 0 / 1617 | 6 | 0.0% | 146 / 25 |
| rtt-100 | 100 ms RTT | 389 | 60 | 6.2 (0) | 108.5 / 110.6 / 118.8 | 0 (0) | 0 / 0 | 0 / 1616 | 6 | 0.0% | 150 / 25 |
| rtt-200 | 200 ms RTT | 698 | 60 | 6.0 (0) | 159.7 / 163.8 / 180.2 | 0 (0) | 0 / 0 | 0 / 1588 | 6 | 0.0% | 149 / 25 |
| jitter | 50 ms RTT, ±10 ms jitter, in order | 241 | 60 | 6.2 (0) | 96.3 / 98.3 / 114.7 | 0 (0) | 0 / 0 | 0 / 1627 | 6 | 0.0% | 150 / 24 |
| reorder-2 | 50 ms RTT, 2% of packets 25 ms early | 221 | 60 | 6.2 (0) | 88.1 / 90.1 / 92.2 | 0 (0) | 0 / 0 | 0 / 1629 | 6 | 0.0% | 145 / 25 |
| reorder | 50 ms RTT, ±10 ms jitter, every packet on its own | 249 | 59 | 5.8 (1) | 96.3 / 98.3 / 100.4 | 1 (250) | 4 / 0 | 2 / 1621 | 8 | 0.0% | 150 / 24 |
| loss-1 | 50 ms RTT, 1% loss | 222 | 60 | 6.2 (0) | 108.5 / 112.6 / 114.7 | 0 (0) | 0 / 0 | 0 / 1630 | 6 | 2.0% | 150 / 25 |
| loss-2 | 50 ms RTT, 2% loss | 237 | 59 | 6.2 (0) | 114.7 / 151.6 / 155.6 | 1 (106) | 0 / 0 | 0 / 1616 | 6 | 6.1% | 147 / 25 |
| loss-5 | 50 ms RTT, 5% loss | 220 | 59 | 6.2 (0) | 147.5 / 167.9 / 167.9 | 0 (0) | 1 / 0 | 0 / 1627 | 6 | 11.2% | 152 / 26 |
| loss-both | 50 ms RTT, 2% loss both ways | 274 | 60 | 6.2 (0) | 114.7 / 129.0 / 151.6 | 1 (106) | 0 / 0 | 0 / 1617 | 6 | 6.2% | 148 / 25 |
| burst | 50 ms RTT, bursts of loss (~4%) | 226 | 60 | 6.2 (0) | 143.4 / 159.7 / 163.8 | 0 (0) | 0 / 0 | 0 / 1630 | 6 | 7.7% | 148 / 26 |
| cap-20 | 50 ms RTT, 20 Mbit/s, 100 ms buffer | 225 | 60 | 6.2 (0) | 88.1 / 90.1 / 94.2 | 0 (0) | 0 / 0 | 0 / 1627 | 6 | 0.0% | 167 / 30 |
| cap-6 | 50 ms RTT, 6 Mbit/s (below the stream) | 239 | 59 | 3.9 (1) | 96.3 / 116.7 / 163.8 | 1 (100) | 5 / 0 | 0 / 1626 | 7 | 0.0% | 161 / 25 |
| cap-drop | 50 ms RTT, 20 Mbit/s, 4 Mbit/s from 5 s to 10 s | 232 | 54 | 3.1 (6) | 208.9 / 221.2 / 254.0 | 5 (1112) | 41 / 9 | 37 / 1621 | 18 | 0.7% | 162 / 22 |

| scenario | first sound, ms | latency p50 / p95 / p99 / max, ms | jitter buffer delay / target, ms | lost / late | FEC / concealed, ms | breaks | kbit/s | pictures apart p50 / p95 / max (mean), ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| local | 35 | 59.4 / 60.4 / 60.4 / 72.9 | 28 / 24 | 0 / 0 | 0 / 0 | 0 | 33.3 | 0.5 / 3.8 / 20.8 (0.7) |
| rtt-20 | 108 | 69.6 / 73.7 / 73.7 / 135.4 | 31 / 24 | 3 / 3 | 0 / 60 | 0 | 33.3 | 0.7 / 3.1 / 62.6 (1.2) |
| rtt-50 | 198 | 81.9 / 81.9 / 84.0 / 95.3 | 26 / 24 | 0 / 0 | 0 / 0 | 0 | 33.3 | 0.7 / 2.8 / 35.0 (1.0) |
| rtt-100 | 348 | 108.5 / 110.3 / 110.6 / 115.2 | 28 / 24 | 0 / 0 | 0 / 0 | 0 | 33.3 | 0.5 / 2.2 / 58.6 (0.7) |
| rtt-200 | 638 | 159.7 / 159.7 / 159.7 / 166.4 | 28 / 24 | 0 / 0 | 0 / 0 | 0 | 33.3 | 0.6 / 3.3 / 72.3 (1.0) |
| jitter | 223 | 94.2 / 96.2 / 96.2 / 105.8 | 57 / 62 | 1 / 1 | 0 / 20 | 0 | 33.3 | 0.6 / 3.0 / 50.3 (0.9) |
| reorder-2 | 208 | 88.1 / 88.1 / 90.1 / 93.5 | 56 / 48 | 0 / 0 | 0 / 0 | 0 | 33.3 | 0.7 / 3.3 / 22.7 (0.8) |
| reorder | 208 | 96.3 / 96.3 / 96.3 / 99.0 | 64 / 66 | 2 / 2 | 0 / 40 | 0 | 33.3 | 0.6 / 3.0 / 127.3 (0.9) |
| loss-1 | 207 | 106.5 / 108.5 / 108.5 / 114.9 | 51 / 55 | 6 / 6 | 120 / 20 | 0 | 33.3 | 1.4 / 20.0 / 40.8 (3.4) |
| loss-2 | 187 | 106.5 / 143.4 / 147.5 / 177.4 | 86 / 82 | 13 / 13 | 120 / 60 | 0 | 33.3 | 4.9 / 17.9 / 75.2 (7.8) |
| loss-5 | 201 | 147.5 / 163.8 / 163.8 / 166.1 | 105 / 99 | 3 / 3 | 60 / 80 | 0 | 33.3 | 1.1 / 8.2 / 74.2 (3.0) |
| loss-both | 198 | 106.5 / 127.0 / 129.0 / 129.7 | 70 / 58 | 15 / 15 | 260 / 80 | 0 | 33.3 | 0.7 / 15.4 / 93.7 (5.0) |
| burst | 201 | 139.3 / 158.8 / 158.8 / 158.8 | 102 / 83 | 7 / 7 | 60 / 160 | 0 | 33.3 | 1.5 / 20.0 / 74.9 (4.3) |
| cap-20 | 207 | 86.0 / 87.6 / 88.1 / 89.5 | 30 / 28 | 0 / 0 | 0 / 0 | 0 | 33.3 | 0.7 / 3.8 / 31.9 (1.1) |
| cap-6 | 208 | 96.3 / 98.3 / 98.3 / 110.6 | 40 / 35 | 2 / 2 | 0 / 40 | 0 | 33.3 | 0.6 / 25.6 / 102.3 (4.3) |
| cap-drop | 199 | 207.7 / 221.2 / 221.2 / 225.1 | 151 / 128 | 18 / 13 | 40 / 440 | 1 | 33.3 | 0.6 / 17.4 / 118.3 (3.1) |

Against stage 2's rows, before → now, ms: the first picture, then the pictures' latency at the
median.

| | local | rtt-20 | rtt-50 | rtt-100 | rtt-200 | jitter | reorder-2 | reorder | loss-1 | loss-2 | loss-5 | loss-both | burst | cap-20 | cap-6 | cap-drop |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| first picture | 55 → 72 | 169 → 145 | 329 → 217 | 638 → 389 | 1201 → 698 | 330 → 241 | 326 → 221 | 306 → 249 | 332 → 222 | 329 → 237 | 338 → 220 | 361 → 274 | 329 → 226 | 334 → 225 | 335 → 239 | 331 → 232 |
| latency p50 | 57 → 59 | 70 → 70 | 88 → 82 | 131 → 109 | 184 → 160 | 104 → 96 | 82 → 88 | 90 → 96 | 121 → 109 | 127 → 115 | 139 → 148 | 121 → 115 | 143 → 143 | 88 → 88 | 109 → 96 | 209 → 209 |

- **Joining a round trip and more sooner** on every path with one (stage 3's fixes: 3.2 (b) 1,
  librats L2 (b)), as their first look predicted. `local` is the one row slower: two runs of
  three at 72–74 ms, one at 57 as before; not looked into.
- **Long round trips hold now.** `rtt-200` had 3 stalls, 51 pictures lost and a break in the
  sound; now none, and the sound's jitter buffer stays at its target (28 ms for 24; it was 51),
  so the pictures, which wait for it, come 25 ms sooner. `rtt-100` the same way, 22 ms.
- **The rest as in stage 2**, within a run's spread: lip sync within 1 ms at the median (5 ms on
  `loss-2`) and 26 ms at p95 at worst (`cap-6`); the sound without a break but on `cap-drop`, a
  path at 4 Mbit/s under a 6 Mbit/s stream for 5 s. `cap-drop` keeps its 209 ms median, the
  sound's delay pushed up and staying (stage 2's last section).
- **CPU** as before but on the three cap rows, run last: 161–167% for the sender (146–150 on the
  others). Not looked into; the VM's other load at the time is as likely as anything.

What stage 6 added rides on the same streams but is not measured here: a camera beside the
pictures (6.5) shares the path with them by their most (`MediaSession` tests, in virtual time);
the voice both ways and its echo cancellation (6.4, 6.6) are tested against WebRTC's processing
and PipeWire on this host. `ratcast-send` carries one picture stream and one sound, so a
shared path is measured here once a tool sends a camera beside a screen.

## A room (10.5)

Stage 10's rooms over netem: `ratcast-room` members, each a `Call` in a mesh as the app's are,
talking with the stamped tone (Opus voice, DTX and FEC, as `publish_voice()` codes a
microphone), each measuring every voice it hears — capture to speaker — and the share it
watches. Each joins through the one before it and meets the rest through them (librats'
peer exchange). One run each, 10 s with everyone in: a small run to close the stage, not a
survey. Voices are the worst member's; breaks, losses and concealment all members' together.

Three members, talking only — `bench/netem.py --room 3 --duration 8 local loss-2`:

| scenario | path | voices heard | all heard after, ms | voice latency p50 / p95 / p99 / max, ms | breaks | lost / concealed, ms | sent, Mbit/s: first / others | CPU, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| local | veth, no shaping | 2 of 2 | 34 | 62.5 / 63.5 / 64.5 / 65.3 | 0 | 0 / 0 | 0.12 / 0.12 | 9 |
| loss-2 | 50 ms RTT, 2% loss | 2 of 2 | 499 | 90.1 / 143.4 / 143.4 / 145.4 | 0 | 45 / 180 | 0.12 / 0.13 | 9 |

Four, the first sharing — `bench/netem.py --room 4 --share --size 1280x720 --bitrate 3000
--fps 30 --duration 10 local rtt-100 loss-2 cap-6`:

| scenario | path | voices heard | all heard after, ms | voice latency p50 / p95 / p99 / max, ms | breaks | lost / concealed, ms | share: first picture, ms | fps | latency p95, ms | stalls | sent, Mbit/s: first / others | CPU, % |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| local | veth, no shaping | 3 of 3 | 43 | 60.4 / 62.5 / 63.5 / 86.2 | 0 | 2 / 40 | 88 | 29 | 22.5 | 0 | 7.70 / 0.20 | 64 |
| rtt-100 | 100 ms RTT | 3 of 3 | 554 | 110.6 / 116.7 / 116.7 / 123.6 | 0 | 0 / 0 | 817 | 27 | 88.1 | 0 | 7.45 / 0.25 | 65 |
| loss-2 | 50 ms RTT, 2% loss | 3 of 3 | 295 | 124.9 / 172.0 / 172.0 / 237.6 | 5 | 72 / 1040 | 381 | 28 | 127.0 | 4 | 8.03 / 0.25 | 65 |
| cap-6 | 50 ms RTT, 6 Mbit/s (below the stream) | 3 of 3 | 448 | 88.1 / 208.0 / 208.0 / 251.0 | 2 | 58 / 1140 | 719 | 25 | 172.0 | 16 | 4.07 / 0.24 | 70 |

- **Everyone meets everyone**, on every row: each hears all the others, those it joined
  through and those it met through them. The last to join hears them all 34–43 ms after it
  starts on the bare link, 0.3–0.6 s with a round trip of 50–100 ms: a connection, the peer
  exchange's question and answer, a dial and its handshake.
- **A voice in a room is a voice in a call:** 60 ms from capture to the speaker on the bare
  link, 111 ms at a 100 ms round trip — stage 6's sound, 59 and 108 ms, one to one.
- **The one who shares sends a copy to each.** 7.7 Mbit/s from the first member for three
  viewers of a 2.5 Mbit/s share; the others send their voice, 0.2 Mbit/s with feedback. A mesh's
  room is as big as its sharer's uplink: stage 5's forwarding is the way past it.
- **The sharer's voice waits behind its pictures** on a narrow uplink. On `cap-6` the others
  hear the sharer at 186 ms, each other at 81–86: its voice queues with three copies of the
  share in the path's 100 ms buffer, which rate control keeps from overflowing but not from
  filling — each viewer's controller sees its own path, not the uplink they share (4.1 Mbit/s
  sent of 6). Nothing librats can order: the queue is the network's.
- **A burst of loss raises a voice's delay for 10 s** (`loss-2`: 85 → 145 ms, and staying):
  the jitter buffer remembers lateness for 10 s (`JitterBuffer::Config::memory`) and shrinks in
  silence, and a tone has none. Speech pauses, so a voice comes back sooner than this one.
- **Not counted:** a first run of the four's `cap-6` had the VM stop every process for 830 ms
  at once (each one's tone fell behind its own clock), its voices breaking together; the row
  is a second run's.


## A sender's budget (5.1)

The copies of a share to several viewers all go through the sharer's uplink, and each viewer's
rate control sees only its own path, which looks as wide as the uplink. `SharedUplink`
(`libratcast/session/shared_uplink.h`) finds the uplink full when every path with copies says
so at once — frames expired, or the round trip risen 25 ms over its least of ten seconds in two
samples running, which comes before the overflow — takes what they carried together over the
last second for what it carries, and keeps the copies to 85% of it, each stream's in proportion
to its most, the most it was found to carry kept for a minute. A viewer joining an uplink known
to be full brings the copies down before its keyframe. And each path with copies is paced to its
part of the uplink (librats L20, `PeerNetwork::set_send_ceiling`), so that a frame's copies,
leaving together, fit it too: without that, three copies each paced by its own controller at
4–5 Mbit/s overfilled a 6 Mbit/s uplink for a moment every frame, and the sharer's voice queued
behind them. ratcast 74ac7c2 + 5.1, librats 529f037 + L20.

On the way, a fault in rate control (1.1) found by it: after a cut at a known ceiling the target
is 85% of it, rounded; rounded down, it sat a hair short of where "quickly back" aims, a step
too small to take — and so no step at all, for good. Half of all cuts at a ceiling left the
bitrate there for the rest of the run. Now from short of it by less than a raise is worth, it
probes on (+8%).

Four in a room, the first sharing — `bench/netem.py --room 4 --share --size 1280x720 --bitrate
3000 --fps 30 --duration 60 cap-6`, one run each:

| build | copies' bitrate after joining | voice latency p50 / p95 / p99 / max, ms | breaks | lost / concealed, ms | share: fps | latency p95, ms | stalls | sent, Mbit/s: first |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| before | 1.07 → 1.47 Mbit/s, then held there (the fault above) | 102.4 / 196.6 / 204.8 / 310.4 | 18 | 141 / 2989 | 29 | 122.9 | 30 | 4.84 |
| the fault fixed, no budget | 1.05 – 3.00 Mbit/s, overfilling the uplink every few seconds | 112.6 / 245.8 / 245.8 / 651.0 | 63 | 889 / 19518 | 28 | 155.6 | 81 | 4.83 |
| the budget | 1.57 Mbit/s from the 10th second on (an uplink of 6.2 found) | 90.1 / 188.4 / 192.5 / 508.0 | 14 | 250 / 5144 | 29 | 100.4 | 10 | 4.93 |
| 5.1: the budget, the paths paced to it, the jitter buffer's excess played out | 1.35 Mbit/s from the 5th second on (5.3 found) | 102.4 / 143.4 / 163.8 / 229.3 | 0 | 22 / 782 | 30 | 96.3 | 1 | 4.34 |

The sharer's voice and the others', as each viewer hears them, seconds 20–60 (the mean of each
second's mean): before 155 and 117 ms, with the budget alone 136 and 89, with all of 5.1 97 and
93.

- **Rate control alone cannot share an uplink**: with the fault fixed and no budget, each copy
  climbs until the uplink overflows — every path's frames expire at once, the voices with them
  — is cut, and climbs again: 81 stalls in a minute and 19.5 s of voice made up.
- **With the budget it is found once and kept**: the third viewer's joining overflows the uplink
  (nothing knew it yet; the second copy's ramp met it the same moment), and from then on the
  copies hold 1.57 Mbit/s each, 4.9 of 6 sent, the paths' round trips back at their least.
- **The budget alone left the sharer's voice slow**, at 140–180 ms against the others' 80–90,
  for two reasons. Its frames did come late: each frame of the share left as three copies at
  once, each paced at its own path's 4–5 Mbit/s, together past the uplink for 10–15 ms a frame,
  and a voice packet in the uplink's queue then waited behind them (its jitter buffer aimed at
  56–60 ms where the others' aimed at 24). Paced to their parts (2 Mbit/s each here), the
  copies leave as the uplink takes them, the frames wait in the sender's own queue — where its
  sound goes first — and the paths' round trips stay at their least (55 ms on a 50 ms path):
  the buffer aims at ~36 ms. And the buffer stayed up to 30 ms over its aim for good: in sound
  that never pauses it gave up a frame only when over by more than a frame and a half
  (`JitterBuffer`); now by more than a frame and a quarter, which a frame fewer still leaves
  over. With both, the sharer is heard as quickly as anyone, 97 against 93 ms.
- **Left: the one overflow before the uplink is known.** The third viewer still overflows it once,
  as it joins: the second copy's ramp meets it the same second. Every voice's buffer then aims
  past 100 ms for the ten seconds it remembers lateness.
- **Nothing else is taken for a full uplink**: `local`, `rtt-100`, `loss-2`, `jitter` and
  `cap-20`, four in a room for 15 s, keep the copies at their most (3 Mbit/s) — the rise of a
  jittery path is over its own least of ten seconds, not over the least it ever had. The breaks
  in those runs are the VM's: every voice breaks together, the viewers' too, whose uplinks
  carry only their voice.
- **The fault fixed, one to one** (`bench/netem.py --repeat 3 cap-6 cap-drop`, before → after):
  `cap-6` 59 → 58 fps, stalls 2 (219 ms) → 5 (619 ms), cuts 2 → 3; `cap-drop` 55 → 52 fps,
  stalls 7 (1405) → 9 (2245), cuts 4 → 6 — it probes past the ceiling, as it was meant to,
  and on `cap-drop` that is the path's 20 Mbit/s found again after its narrow five seconds:
  before, two runs of three stayed at 3.1–3.4 Mbit/s to the end; now they climb back to 7–8.

## Playing in step (5.2)

A share's viewers each played it as soon as they could: on paths of different lengths a film
was seen at as many moments as there were viewers. Now each viewer measures its clock against
the clock the sharer's frames are captured on — probes on the media lane, answered at once, the
round trip that took least of the last ten seconds (`PeerClock`) — and tells the sharer what it
needs from capture to playing, on the sharer's clock: the more of its sound's and its pictures'
own needs, never what they play at. The sharer tells them all the most of it and 10 ms
(`CommonDelay`): up at once, down only once every need has stayed lower for five seconds. The
viewers' `PlayoutClock`s play to it — the sound's floor raised to it, the pictures held to the
sound, or to it where there is no sound. ratcast 9628ea6 + 5.2, librats d4558e7.

Four in a room on `local`, the first sharing; the three viewers each on a path of its own —
`bench/netem.py --room 4 --share --size 1280x720 --fps 30 --bitrate 3000 --duration 30 --repeat 3
--member-paths local,rtt-100,loss-2 local`, each viewer apart (`--send-arg=--apart`) and in step:

| viewers | picture latency p50, ms: `local` / `rtt-100` / `loss-2` | apart, ms | every viewer at, ms | picture latency p95, ms | stalls (all viewers) | voice latency p50, ms: the worst member's |
|---|---:|---:|---:|---:|---:|---:|
| each in its own time | 18.9 / 81.9 / 104.4 | 84.4 | – | 108.5 | 1 | 108.5 |
| in step | 122.9 / 122.9 / 122.9 | 0.0 | 156 | 159.7 | 7 | 135.2 |

- **The viewers show a frame together**: their medians fall in one bucket of the latency
  histogram (3%, 4 ms here), where they were 85 ms apart. What it costs is the slowest viewer's
  latency and the margin for all: the one on `local` shows the share 100 ms later than it could.
- **The stalls are the VM's**, which stops now and then for 100 ms or more: every member's
  voice breaks at the same moment then, the sharer's included, and so do the pictures. In step,
  each such moment also raises the slowest viewer's need, which raises everyone's delay (126 →
  156 ms in one run, at the 29th second, kept for the rest of it) and holds every viewer's
  pictures once more for it. The voices are not held to the share; their latency moves with the
  VM between runs (the worst member's p50 108–143 ms in both).
- The delay is held within a stream's lifetime: a viewer holds pictures and sound no longer than
  that past its own time, and one whose own path is that much quicker than the slowest's stays
  ahead of the others by the rest.

## Forwarding tree (5.3)

A share's copies all leave through the sharer's uplink (5.1 keeps them to their part of it). Now,
once that uplink is found short — each copy of a picture stream getting less than ¾ of its most
— the sharer hands viewers to other viewers, which pass the frames on as they came, each with the
time it has left (librats L5): the last viewer to come that passes nothing on goes through the
subscriber the fewest hops away, passing on to the fewest, then the nearest, until no copy is
short or one is left. Sound stays direct. A relay is taken only if a frame through it arrives in
time with a repair on its last hop — what it takes to reach the relay, half the relay's round trip
to the viewer and one more, within ¾ of the frame's lifetime — judged by the least round trips of
the last ten seconds (a queue is rate control's to empty, not a longer path): before the hand-off
by the sharer's own round trips (the one between two viewers at least their difference), after it
by the relay's word (`RelayedPath`). ratcast 83e3379 + 5.3, librats fd21fa1.

Four in a room, the first sharing 1080p60 (av1, 8 Mbit/s at most) through an uplink of 6 Mbit/s
— one copy wide — the others' uplinks free: `bench/netem.py --room 4 --share --member-paths
local,local,local cap-6 --duration 60`, all copies from the sharer (`--send-arg=--no-relay`) and
through the tree; then the viewers each on a path of its own (`--member-paths
rtt-100,local,loss-2`). One run each:

| viewers' paths | copies | the tree | aim, Mbit/s | voice latency p50 / p95 / p99 / max, ms | breaks | lost / concealed, ms | share: fps | latency p95, ms | stalls (all viewers) | sent, Mbit/s: sharer / most of the others |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| `local` ×3 | from the sharer | – | 0.75 – 1.0 | 63.5 / 129.0 / 217.1 / 890.7 | 28 | 514 / 11574 | 55 | 176.1 | 71 | 4.17 / 0.25 |
| `local` ×3 | the tree, the copy held to the uplink first found | m0 → m1 → {m2, m3} | 3.34 | 63.5 / 131.1 / 131.1 / 160.7 | 1 | 8 / 245 | 59 | 106.5 | 9 | 3.16 / 5.91 |
| `local` ×3 | the tree | m0 → m1 → {m2, m3} | 3.1 → 8, cut every 7 s | 63.5 / 127.0 / 143.4 / 266.8 | 25 | 144 / 3007 | 58 | 147.5 | 24 | 4.36 / 8.26 |
| `rtt-100`, `local`, `loss-2` | from the sharer | – | 0.8 – 1.0 | 139.3 / 172.0 / 212.4 / 212.4 | 1 | 166 / 512 | 53 | 200.7 | 58 | 4.37 / 0.26 |
| `rtt-100`, `local`, `loss-2` | the tree, no word of time | m0 → m1 → {m2, m3} | 4.9 – 7.9 | 143.4 / 188.4 / 208.6 / 208.6 | 0 | 177 / 402 | 2 | 385.0 | 204 | 5.04 / 5.73 |
| `rtt-100`, `local`, `loss-2` | the tree | m0 → {m1, m2 → m3} | 1.95 | 131.1 / 172.0 / 229.4 / 238.0 | 16 | 301 / 3082 | 58 | 213.0 | 15 | 3.88 / 2.22 |

- **Three copies through one copy's uplink do not play**: each gets 0.8–1 Mbit/s of a 1080p60
  share, the frames lost to the overflows ask for keyframes, a keyframe a second, and every
  viewer stalls (71 in a minute) — the sharer's voice with them (11.6 s made up).
- **Through the tree every viewer plays at 58–59 fps**, the sharer sending one copy and the
  first viewer passing two on, in step with each other (apart 0–2 ms). From the third second:
  the third viewer's joining finds the uplink full, and the second and third go through the
  first, the fewest hops away.
- **The one copy left follows its own path.** The uplink was found full while the copies were
  starting, at 4.06 Mbit/s of its 6; kept to its part of that, the one copy left from the sharer
  stayed at 3.34 Mbit/s for the minute the uplink is remembered — and on from there, the uplink
  forgotten (a single path never finds it again: 100 s with that rule, 59 fps, 31 stalls, from
  the 63rd second the same as below). One path alone is its own, as `SharedUplink` says: it is
  now left to its rate control at once, which takes it to 4.4 Mbit/s on average — and on past
  the 6, the target climbing to the stream's most while the encoder sends 4–5.5 of it, cut where
  frames expire, every 7 s, and every member hearing the sharer's voice break as it does: 24
  stalls in a minute where the held copy had 9. That is a single path through a narrow uplink,
  the tree's or a call's (1.1): left for rate control.
- **A relay is chosen for the time it leaves.** With no word of time, the first viewer —
  100 ms from the others, 75 from the sharer — was handed both the others, by the fewest hops:
  its frames reached them 125–150 ms after capture of the 200 they live, with no time left to
  repair the 2% that `loss-2` drops; keyframes were asked of the sharer four times a second and
  the third viewer saw 2 fps. Now the sharer passes it over (its round trip less the others' is
  too long for a repair) and hands the third viewer to the second, 25 ms from it: two copies
  from the sharer, every viewer at 58–59 fps. Judged first by the smoothed round trips, the
  sharer's own queue in a keyframe's burst (100 ms and more on `cap-6`) made that relay look too
  far for good three seconds in; by the least round trips it stays.
- **Left**: an uplink found full while the copies start is found narrower than it is (4.06 and
  4.6 of 6 here) and the copies held to it for a minute; with two or more paths it is found
  again after that.

## Probing past a ceiling (1.1, after 5.3)

The one copy left from the sharer, through the tree on `cap-6`, sawed: up to the stream's most,
cut where frames expired, every 7 s. Two faults of rate control's, a single path's anywhere.
After a cut at a full path's ceiling it probed on, +8% a second, and took a target 15% past the
ceiling with no trouble for a path grown — but an encoder sends what the picture needs (SVT-AV1
on the pattern: 70–80% of its target), so the path was never tried at that target: it ramped ×1.5
to the most, and the encoder filled the path at once as the picture came to need more. And it
probed every second whatever it had found: past the same 5.85 Mbit/s, cut, back, past it
again. Now a path is taken for grown only once it carried well past the ceiling (`carried`,
over the last second, the relays telling it in `RelayedPath`), and a ceiling found again where
it was — what the path carried at the cut no less than 85% of it — has the steps past it twice
as far apart each time, up to 8 s; one narrower, or grown, every second again. ratcast d8a6a2c +
this, librats fd21fa1.

Four in a room, the sharer's uplink one copy wide (`bench/netem.py --room 4 --share
--member-paths local,local,local cap-6 --duration 60`), the two builds in turn, three runs each:

| build | cuts / raises / keyframes, a minute | share: fps | latency p95, ms | stalls (by viewer) | sent, Mbit/s: sharer / the relay |
|---|---:|---:|---:|---:|---:|
| before | 9 / 41 / 58 | 58 | 151.6 | 17 (7, 5, 5) | 4.66 / 8.87 |
| before | 9 / 41 / 58 | 58 | 147.5 | 34 (12, 11, 11) | 4.39 / 8.30 |
| before | 8 / 44 / 59 | 58 | 151.6 | 23 (7, 9, 7) | 4.67 / 8.88 |
| after | 5 / 20 / 31 | 59 | 135.2 | 20 (9, 6, 5) | 4.73 / 8.99 |
| after | 6 / 21 / 33 | 59 | 124.9 | 16 (6, 5, 5) | 4.61 / 8.77 |
| after | 5 / 24 / 35 | 59 | 143.4 | 19 (5, 7, 7) | 4.69 / 8.93 |

- **Half the cuts, half the raises**: the steps past 5.85 Mbit/s come 2, then 4, then 8 s apart,
  four of them from a cut to the next (4.75 → 6.5, the encoder sending 4–5.5): at 8 s apart, a
  cut every half a minute or so where it was every 7 s. Past the 20th second the target went
  to the stream's most in every cycle before; after, once in three runs — the first ceiling,
  found while the viewers joined, too low (4.7), the path carrying 5.5 past it: found grown, as
  it was, and the right one (5.8) found by the cut that followed. More of the path used, not
  less: 4.4–4.5 Mbit/s sent past the 20th second where it was 4.0–4.4.
- **Each raise costs a keyframe here** (SVT-AV1 changes its bitrate only by opening anew): a gap
  of 65–100 ms between two pictures at the viewers, not a stall. A frame is lost only at a cut;
  with half the raises, half those gaps too.
- **Stalls are the VM's as much as rate control's**: the same build gives 17 and 34. The
  voice breaks (0–25 a run) come with the whole machine stopping for 300–450 ms, every
  voice at once; the sound goes ahead of the pictures in the sender's queue (librats L4).
- **One to one** (`--repeat 2 --duration 30 local cap-20 cap-6 cap-drop`, medians, before →
  after): on `local` and `cap-20` the most (8 Mbit/s) in 6 s and held, as before, 7.5–7.7
  Mbit/s sent; `cap-6` 58 → 59 fps, 4.2 (4 cuts) → 4.3 (3), stalls 2 (354 ms) → 0 (52), the
  target 4.7–8.0 → 4.7–7.3, keyframes 26 → 18; `cap-drop` 56 → 57 fps, stalls 8 (1864) → 10
  (1872), but back at the most 4–8 s later: 13 and 17 s → 21 and 17 s, the path wide again from
  the 10th. That is the price: a path grown is found only as it carries well past the ceiling,
  and an encoder sending 75–80% of its target needs the target some 30–40% past it for that,
  +8% a second. (15 s, three runs each, `local`, `jitter`, `loss-2`, `cap-6`, `cap-drop`: the
  same within the VM's noise.)
- **Tried and left out**: no step past a ceiling further than one past what the paths carried
  — a target held just past the 5.85 with no cut for 40 s — but a ceiling found too low (a cut
  while the machine stalled: 3.85) then held for good, the encoder sending too little ever to
  carry past it. A cut on the round trip rising (as `SharedUplink`'s): it never rose, as librats
  paces each path to its estimate and the queue is the sender's own, 30–75 ms, the network's
  empty. And steps past the ceiling only while the encoder sends 80% of its target: SVT-AV1 sits
  at about that, and nothing changed.
