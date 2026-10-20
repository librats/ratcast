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
