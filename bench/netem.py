#!/usr/bin/env python3
"""ratcast-send -> ratcast-recv across an emulated network.

The sender runs in one network namespace and the viewer in another, joined by a
veth pair whose two directions are shaped apart with tc netem: delay, jitter,
loss, bandwidth. No root needed: the namespaces belong to a user namespace of
our own. The kernel has to allow that (kernel.unprivileged_userns_clone=1; on
Ubuntu 24.04+ also kernel.apparmor_restrict_unprivileged_userns=0).

Every run starts a fresh sender and a fresh viewer -- a new connection and a new
encoder -- so the wait for the first picture, joining, is measured each time.
The sender sends the stamped test pattern, so the viewer reads the latency from
capture to decoded picture off every picture.

    bench/netem.py                            # every scenario, once
    bench/netem.py --repeat 3 rtt-100 loss-2  # some of them, three times each
    bench/netem.py --list

Prints a Markdown table (medians over the repeats); the tools' own output for
every run is kept in --out.
"""

import argparse
import os
import re
import signal
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SENDER_ADDRESS = "10.77.0.1"
VIEWER_ADDRESS = "10.77.0.2"
PORT = 7700


@dataclass
class Scenario:
    name: str
    path: str  # what it stands for, for the table
    forward: str = ""  # netem options, sender -> viewer
    backward: str = ""  # netem options, viewer -> sender


def queue_limit(rate_mbit, delay_ms, buffer_ms=100):
    """Packets netem may hold: those in flight over the delay plus a buffer of
    `buffer_ms` at the rate. netem counts both against one limit."""
    packets_per_ms = rate_mbit * 1e6 / 8 / 1220 / 1000
    return max(16, int(packets_per_ms * (delay_ms + buffer_ms)))


def capped(rate_mbit, delay_ms):
    return f"delay {delay_ms}ms rate {rate_mbit}mbit limit {queue_limit(rate_mbit, delay_ms)}"


# Delays are one way: a scenario's round trip is twice its delay. Loss is on the
# media's direction only unless the scenario says otherwise.
SCENARIOS = [
    Scenario("local", "veth, no shaping"),
    Scenario("rtt-20", "20 ms RTT", "delay 10ms", "delay 10ms"),
    Scenario("rtt-50", "50 ms RTT", "delay 25ms", "delay 25ms"),
    Scenario("rtt-100", "100 ms RTT", "delay 50ms", "delay 50ms"),
    Scenario("rtt-200", "200 ms RTT", "delay 100ms", "delay 100ms"),
    # netem keeps packets in order when it also paces them (rate), as a queue
    # that varies does; without it each packet's delay is drawn on its own.
    Scenario("jitter", "50 ms RTT, ±10 ms jitter, in order",
             "delay 25ms 10ms distribution normal rate 1gbit",
             "delay 25ms 10ms distribution normal rate 1gbit"),
    Scenario("reorder-2", "50 ms RTT, 2% of packets 25 ms early", "delay 25ms reorder 2%",
             "delay 25ms"),
    Scenario("reorder", "50 ms RTT, ±10 ms jitter, every packet on its own",
             "delay 25ms 10ms distribution normal", "delay 25ms 10ms distribution normal"),
    Scenario("loss-1", "50 ms RTT, 1% loss", "delay 25ms loss 1%", "delay 25ms"),
    Scenario("loss-2", "50 ms RTT, 2% loss", "delay 25ms loss 2%", "delay 25ms"),
    Scenario("loss-5", "50 ms RTT, 5% loss", "delay 25ms loss 5%", "delay 25ms"),
    Scenario("loss-both", "50 ms RTT, 2% loss both ways", "delay 25ms loss 2%",
             "delay 25ms loss 2%"),
    # Gilbert-Elliott: 1% chance to enter a bad state, 25% to leave it, all
    # lost while in it -- ~4% loss in bursts of 4 on average.
    Scenario("burst", "50 ms RTT, bursts of loss (~4%)", "delay 25ms loss gemodel 1% 25%",
             "delay 25ms"),
    Scenario("cap-20", "50 ms RTT, 20 Mbit/s, 100 ms buffer", capped(20, 25), "delay 25ms"),
    Scenario("cap-6", "50 ms RTT, 6 Mbit/s (below the stream)", capped(6, 25), "delay 25ms"),
]


def sh(*command, check=True):
    return subprocess.run(command, check=check, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True)


def in_viewer(*command):
    return ["ip", "netns", "exec", "viewer", *command]


def make_network():
    """Two namespaces joined by veth: this one (the sender's) and 'viewer'."""
    # `ip netns` keeps its names under /run/netns, which we may not create:
    # then /run itself is replaced, in our mount namespace only.
    try:
        Path("/run/netns").mkdir(exist_ok=True)
    except PermissionError:
        sh("mount", "-t", "tmpfs", "none", "/run")
        Path("/run/netns").mkdir()
    sh("mount", "-t", "tmpfs", "none", "/run/netns")
    sh("ip", "netns", "add", "viewer")
    sh("ip", "link", "add", "va", "type", "veth", "peer", "name", "vb", "netns", "viewer")
    sh("ip", "addr", "add", f"{SENDER_ADDRESS}/24", "dev", "va")
    sh("ip", "link", "set", "va", "up")
    sh("ip", "link", "set", "lo", "up")
    sh(*in_viewer("ip", "addr", "add", f"{VIEWER_ADDRESS}/24", "dev", "vb"))
    sh(*in_viewer("ip", "link", "set", "vb", "up"))
    sh(*in_viewer("ip", "link", "set", "lo", "up"))
    # Each end knows the other's MAC address from the start: the first packet
    # of a run would otherwise wait for ARP across the shaped link, a round
    # trip no real path to a gateway pays.
    def mac(*show):
        return re.search(r"link/ether (\S+)", sh(*show).stdout).group(1)

    sh("ip", "neigh", "replace", VIEWER_ADDRESS, "lladdr",
       mac(*in_viewer("ip", "-o", "link", "show", "vb")), "dev", "va", "nud", "permanent")
    sh(*in_viewer("ip", "neigh", "replace", SENDER_ADDRESS, "lladdr",
                  mac("ip", "-o", "link", "show", "va"), "dev", "vb", "nud", "permanent"))


def shape(scenario):
    for device, options, wrap in (("va", scenario.forward, lambda c: c),
                                  ("vb", scenario.backward, lambda c: in_viewer(*c))):
        sh(*wrap(["tc", "qdisc", "del", "dev", device, "root"]), check=False)
        if options:
            sh(*wrap(["tc", "qdisc", "add", "dev", device, "root", "netem", *options.split()]))


def parse_summary(text):
    for line in text.splitlines():
        if line.startswith("summary "):
            return {k: float(v) for k, v in re.findall(r"(\w+)=(-?[\d.]+)", line)}
    return None


def run_once(scenario, args, log_dir, index):
    shape(scenario)
    stem = log_dir / f"{scenario.name}.{index}"
    with open(f"{stem}.send.log", "w+") as send_log:
        sender = subprocess.Popen(
            [str(args.bin / "ratcast-send"), "--source", "pattern", "--size", args.size,
             "--bitrate", str(args.bitrate), "--fps", str(args.fps), "--latency",
             str(args.latency), "--codec", args.codec, "--port", str(PORT), *args.send_args],
            stdout=send_log, stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic() + 15
            while "viewers connect" not in Path(send_log.name).read_text():
                if sender.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"the sender did not start, see {send_log.name}")
                time.sleep(0.05)
            # A viewer joins a stream that is already going, as in a real call:
            # not within the sender's first keyframe interval.
            time.sleep(args.join_after)
            viewer = subprocess.run(
                in_viewer(str(args.bin / "ratcast-recv"), "--connect", f"{SENDER_ADDRESS}:{PORT}",
                          "--headless", "--duration", str(args.duration)),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                timeout=args.duration + 30)
            Path(f"{stem}.recv.log").write_text(viewer.stdout)
        finally:
            sender.send_signal(signal.SIGINT)
            try:
                sender.wait(timeout=15)
            except subprocess.TimeoutExpired:
                sender.kill()
                sender.wait()
    return parse_summary(Path(f"{stem}.send.log").read_text()), parse_summary(viewer.stdout)


def median(runs, side, key):
    values = [r[side][key] for r in runs if r[side] and key in r[side]]
    return statistics.median(values) if values else None


def cell(value, digits=0):
    return "–" if value is None else f"{value:.{digits}f}"


def table(results, args):
    rows = [
        f"{args.size} at {args.fps} fps, {args.codec}, {args.bitrate} kbit/s, frame lifetime "
        f"{args.latency} ms; {args.duration} s a run, medians of {args.repeat}.",
        "",
        "| scenario | path | first picture, ms | fps | latency p50 / p95 / p99, ms "
        "| stalls (ms) | lost / dropped | expired / sent | keyframes | repair | CPU send / recv, % |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for scenario, runs in results:
        first = median(runs, 1, "first_picture_ms")
        fps = median(runs, 1, "fps")
        rows.append(
            f"| {scenario.name} | {scenario.path} | {cell(first)} | {cell(fps)} "
            f"| {cell(median(runs, 1, 'latency_p50_ms'), 1)} / "
            f"{cell(median(runs, 1, 'latency_p95_ms'), 1)} / "
            f"{cell(median(runs, 1, 'latency_p99_ms'), 1)} "
            f"| {cell(median(runs, 1, 'stalls'))} ({cell(median(runs, 1, 'stalled_ms'))}) "
            f"| {cell(median(runs, 1, 'lost'))} / {cell(median(runs, 1, 'dropped'))} "
            f"| {cell(median(runs, 0, 'live_expired'))} / {cell(median(runs, 0, 'live_sent'))} "
            f"| {cell(median(runs, 0, 'keyframes'))} "
            f"| {cell(100 * (median(runs, 0, 'repair_share') or 0), 1)}% "
            f"| {cell(median(runs, 0, 'cpu'))} / {cell(median(runs, 1, 'cpu'))} |")
    return "\n".join(rows)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenarios", nargs="*", help="names (default: all)")
    parser.add_argument("--list", action="store_true", help="list the scenarios")
    parser.add_argument("--bin", type=Path, default=ROOT / "build" / "release" / "bin")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "netem",
                        help="where the tools' output goes")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--duration", type=int, default=15, help="seconds a run")
    parser.add_argument("--size", default="1920x1080")
    parser.add_argument("--bitrate", type=int, default=8000, help="kbit/s")
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--latency", type=int, default=200, help="frame lifetime, ms")
    parser.add_argument("--codec", default="av1")
    parser.add_argument("--join-after", type=float, default=1.0,
                        help="seconds the sender streams before the viewer joins")
    parser.add_argument("--send-arg", dest="send_args", action="append", default=[],
                        help="one more option for ratcast-send (repeatable)")
    args = parser.parse_args()

    if args.list:
        for s in SCENARIOS:
            print(f"{s.name:10} {s.path}\n{'':10}   -> {s.forward or '-'}\n{'':10}   <- "
                  f"{s.backward or '-'}")
        return 0
    unknown = set(args.scenarios) - {s.name for s in SCENARIOS}
    if unknown:
        parser.error(f"no such scenario: {', '.join(sorted(unknown))} (see --list)")
    for tool in ("ratcast-send", "ratcast-recv"):
        if not (args.bin / tool).exists():
            parser.error(f"no {tool} in {args.bin}: build the release preset, or pass --bin")

    # Into namespaces of our own: the process starts over inside them.
    if os.environ.get("RATCAST_NETEM_INSIDE") != "1":
        os.environ["RATCAST_NETEM_INSIDE"] = "1"
        command = ["unshare", "--user", "--map-root-user", "--net", "--mount",
                   "--propagation", "private", sys.executable, *sys.argv]
        os.execvp(command[0], command)

    make_network()
    log_dir = args.out / time.strftime("%Y%m%d-%H%M%S")
    log_dir.mkdir(parents=True)
    chosen = [s for s in SCENARIOS if not args.scenarios or s.name in args.scenarios]
    results = []
    for scenario in chosen:
        runs = []
        for index in range(args.repeat):
            send, recv = run_once(scenario, args, log_dir, index)
            runs.append((send, recv))
            first = recv.get("first_picture_ms") if recv else None
            print(f"{scenario.name} #{index + 1}: first picture {cell(first)} ms, "
                  f"p95 {cell(recv.get('latency_p95_ms') if recv else None, 1)} ms, "
                  f"stalls {cell(recv.get('stalls') if recv else None)}", file=sys.stderr)
        results.append((scenario, runs))
    print(table(results, args))
    print(f"\nlogs: {log_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
