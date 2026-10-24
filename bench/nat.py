#!/usr/bin/env python3
"""ratcast-send -> ratcast-recv, each behind a NAT of its own, meeting at a
ratcast-server between them.

Network namespaces in a user namespace of our own, as bench/netem.py
makes them (no root): this one is the internet, a bridge the server listens
on; each side's NAT is a namespace on that bridge masquerading (nftables) for
the side's host, a namespace of its own behind it. Each NAT is one of:

  cone       the mapping kept for every destination (Linux's masquerade keeps
             the port where it can), what comes in let through only from where
             the host sent to: a port-restricted cone, what home routers are
  symmetric  a new port for each destination (masquerade random): what no
             punch gets through, which some carrier-grade NATs are

The viewer dials the sender by peer id, which both registered at the server
alone (--no-dht): told of each other there, they dial at once, and where that
does not get through the server carries them. The table says whether the
viewer got pictures, how soon, and whether straight or through the server.

With --viewers N there are N viewers, each behind a NAT of its own of the
pair's second kind. Where the server carries their connections it forwards
the share to them too (an SFU): the sender sends it one copy instead of one
each. The table says what the sender's uplink carried, and to how many the
server forwarded.

    bench/nat.py                          # every pair of NATs, once
    bench/nat.py cone-symmetric --repeat 3
    bench/nat.py cone-symmetric --viewers 3
    bench/nat.py cone-cone --viewers 3 --sender-uplink 5   # an uplink short of three copies

Each one-way hop through the internet is delayed --delay ms (default 10), so
the sides are 4 x that apart and the server half of it from each.
"""

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ beside netem.py
sys.path.insert(0, str(Path(__file__).resolve().parent))
from netem import (INTERRUPTIBLE, ROOT, capped, cell, netns_dir, parse_summary, sh, stop,  # noqa: E402
                   tool)

SERVER_ADDRESS = "203.0.113.1"
NAT_ADDRESS = "203.0.113.{}"  # side i's NAT on the internet: .11, .12
HOST_ADDRESS = "10.{}.0.2"  # side i's host behind it, its NAT at 10.i.0.1
KINDS = ("cone", "symmetric")
PAIRS = [f"{a}-{b}" for i, a in enumerate(KINDS) for b in KINDS[i:]]


def in_ns(name, *command):
    return ["ip", "netns", "exec", name, *command]


def make_network(kinds, delay_ms, sender_uplink=0):
    """The internet (this namespace: a bridge, the server on it), and for each
    side a NAT of `kinds[i]` and a host behind it; the first side's way out
    `sender_uplink` Mbit/s wide, if given."""
    netns_dir()
    sh("ip", "link", "add", "internet", "type", "bridge")
    sh("ip", "addr", "add", f"{SERVER_ADDRESS}/24", "dev", "internet")
    sh("ip", "link", "set", "internet", "up")
    sh("ip", "link", "set", "lo", "up")
    for i, kind in enumerate(kinds):
        nat, host = f"nat{i}", f"host{i}"
        sh("ip", "netns", "add", nat)
        sh("ip", "netns", "add", host)
        # The NAT's side of the internet, and the bridge's: each way delayed.
        sh("ip", "link", "add", f"w{i}", "type", "veth", "peer", "name", "wan", "netns", nat)
        sh("ip", "link", "set", f"w{i}", "master", "internet", "up")
        sh("tc", "qdisc", "add", "dev", f"w{i}", "root", "netem", "delay", f"{delay_ms}ms")
        sh(*in_ns(nat, "ip", "addr", "add", f"{NAT_ADDRESS.format(11 + i)}/24", "dev", "wan"))
        sh(*in_ns(nat, "ip", "link", "set", "wan", "up"))
        out = (capped(sender_uplink, delay_ms) if i == 0 and sender_uplink else
               f"delay {delay_ms}ms")
        sh(*in_ns(nat, "tc", "qdisc", "add", "dev", "wan", "root", "netem", *out.split()))
        # The host behind it.
        sh("ip", "link", "add", "lan", "netns", nat, "type", "veth", "peer", "name", "eth0",
           "netns", host)
        sh(*in_ns(nat, "ip", "addr", "add", f"10.{i}.0.1/24", "dev", "lan"))
        sh(*in_ns(nat, "ip", "link", "set", "lan", "up"))
        sh(*in_ns(nat, "ip", "link", "set", "lo", "up"))
        sh(*in_ns(host, "ip", "addr", "add", f"{HOST_ADDRESS.format(i)}/24", "dev", "eth0"))
        sh(*in_ns(host, "ip", "link", "set", "eth0", "up"))
        sh(*in_ns(host, "ip", "link", "set", "lo", "up"))
        sh(*in_ns(host, "ip", "route", "add", "default", "via", f"10.{i}.0.1"))
        # The NAT itself.
        sh(*in_ns(nat, "sysctl", "-qw", "net.ipv4.ip_forward=1"))
        sh(*in_ns(nat, "nft", "add", "table", "ip", "nat"))
        sh(*in_ns(nat, "nft", "add", "chain", "ip", "nat", "post",
                  "{ type nat hook postrouting priority 100 ; }"))
        sh(*in_ns(nat, "nft", "add", "rule", "ip", "nat", "post", "oifname", "wan",
                  "masquerade", *(["random"] if kind == "symmetric" else [])))


def server_stats(log):
    """The server's last word: connections it relays, MB relayed, viewers it
    forwards to, MB forwarded."""
    lines = [line for line in Path(log).read_text().splitlines() if line.startswith("peers ")]
    match = re.search(r"relaying (\d+) \(([\d.]+) MB.*forwarding \d+ to (\d+) \(([\d.]+) MB",
                      lines[-1]) if lines else None
    if not match:
        return {}
    return {"relaying": int(match[1]), "relayed_mb": float(match[2]),
            "forwarding_to": int(match[3]), "forwarded_mb": float(match[4])}


def sent_bytes(host):
    """Bytes `host` has sent so far."""
    show = in_ns(host, "ip", "-j", "-s", "link", "show", "dev", "eth0")
    return json.loads(sh(*show).stdout)[0]["stats64"]["tx"]["bytes"]


def run_once(pair, args, log_dir, index):
    stem = log_dir / f"{pair}.{index}"
    verbose = ["--verbose"] if args.verbose else []
    with open(f"{stem}.server.log", "w") as server_log, \
            open(f"{stem}.send.log", "w+") as send_log:
        server = subprocess.Popen(
            [str(tool(args.bin, "ratcast-server")), "--bind", SERVER_ADDRESS, "--stats-every",
             "1", *verbose],
            stdout=server_log, stderr=subprocess.STDOUT, text=True, **INTERRUPTIBLE)
        sender = None
        try:
            sender = subprocess.Popen(
                in_ns("host0", str(tool(args.bin, "ratcast-send")), "--source", "pattern",
                      "--size", args.size, "--bitrate", str(args.bitrate), "--fps",
                      str(args.fps), "--announce", "--no-dht", "--server", SERVER_ADDRESS,
                      *args.send_args, *verbose),
                stdout=send_log, stderr=subprocess.STDOUT, text=True, **INTERRUPTIBLE)
            deadline = time.monotonic() + 15
            peer = None
            while peer is None:
                for line in Path(send_log.name).read_text().splitlines():
                    if line.startswith("peer "):
                        peer = line.split()[1]
                if sender.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"the sender did not start, see {send_log.name}")
                time.sleep(0.05)
            sent_before, started = sent_bytes("host0"), time.monotonic()
            viewers = [subprocess.Popen(
                in_ns(f"host{i}", str(tool(args.bin, "ratcast-recv")), f"rats://{peer}",
                      "--headless", "--server", SERVER_ADDRESS, "--no-dht", "--duration",
                      str(args.duration), *verbose),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                for i in range(1, args.viewers + 1)]
            outputs = [viewer.communicate(timeout=args.duration + 30)[0] for viewer in viewers]
            uplink_mbps = (sent_bytes("host0") - sent_before) * 8 / 1e6 / \
                (time.monotonic() - started)
            server_said = server_stats(server_log.name)
        finally:
            if sender:
                stop(sender)
            stop(server)
    for i, output in enumerate(outputs, start=1):
        Path(f"{stem}.recv{i}.log").write_text(output)
    return {"viewers": [parse_summary(output) or {} for output in outputs],
            "uplink_mbps": uplink_mbps, **server_said}


def table(results):
    rows = ["| NATs | viewers | got pictures | first picture, ms (worst) | connected "
            "| fps (least) | Mbit/s each (least) | stalls | sender's uplink, Mbit/s "
            "| the server forwards to | it relays, MB |",
            "|---|---|---|---|---|---|---|---|---|---|---|"]
    for pair, runs in results:
        for run in runs:
            viewers = run.get("viewers", [])
            got = [v for v in viewers if v.get("pictures", 0) > 0]
            relayed = sum(1 for v in got if v.get("relayed"))
            way = ("–" if not got else
                   ", ".join(part for part in (f"{len(got) - relayed} straight" if
                                               len(got) > relayed else "",
                                               f"{relayed} through the server" if relayed
                                               else "") if part))
            worst = max((v.get("first_picture_ms", 0) for v in got), default=None)
            least = min((v.get("fps", 0) for v in got), default=None)
            least_mbps = min((v.get("mbps", 0) for v in got), default=None)
            stalls = sum(v.get("stalls", 0) for v in got) if got else None
            rows.append(f"| {pair} | {len(viewers)} | {len(got)} | {cell(worst)} | {way} | "
                        f"{cell(least, 1)} | {cell(least_mbps, 2)} | {cell(stalls)} | "
                        f"{cell(run.get('uplink_mbps'), 1)} "
                        f"| {cell(run.get('forwarding_to'))} | {cell(run.get('relayed_mb'), 1)} |")
    return "\n".join(rows)


def parse_arguments(argv):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pairs", nargs="*", help=f"of {', '.join(PAIRS)} (default: all)")
    parser.add_argument("--bin", type=Path, default=ROOT / "build" / "release" / "bin")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "nat",
                        help="where the tools' output goes")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--duration", type=int, default=15, help="seconds the viewer watches")
    parser.add_argument("--delay", type=int, default=10, help="ms, each hop each way")
    parser.add_argument("--size", default="1280x720")
    parser.add_argument("--bitrate", type=int, default=3000, help="kbit/s")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--viewers", type=int, default=1,
                        help="viewers, each behind a NAT of its own of the pair's second kind")
    parser.add_argument("--sender-uplink", type=int, default=0, metavar="MBIT",
                        help="the sender's way out this wide (default: as wide as it goes)")
    parser.add_argument("--send-arg", dest="send_args", action="append", default=[],
                        help="one more option for ratcast-send (repeatable)")
    parser.add_argument("--verbose", action="store_true", help="the tools' library logs")
    # One run, in namespaces made for it: PAIR INDEX LOG_DIR (the script calls
    # itself so); its outcome, JSON, the last line out.
    parser.add_argument("--inside", nargs=3, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if not 1 <= args.viewers <= 8:
        parser.error("--viewers: 1 to 8")
    unknown = set(args.pairs) - set(PAIRS)
    if unknown:
        parser.error(f"no such pair: {', '.join(sorted(unknown))} (of {', '.join(PAIRS)})")
    for name in ("ratcast-send", "ratcast-recv", "ratcast-server"):
        if not tool(args.bin, name).exists():
            parser.error(f"no {name} in {args.bin}: build the release preset, or pass --bin")
    return args


def main():
    args = parse_arguments(sys.argv[1:])
    if args.inside:
        pair, index, log_dir = args.inside
        first, rest = pair.split("-")
        make_network([first] + [rest] * args.viewers, args.delay, args.sender_uplink)
        print(json.dumps(run_once(pair, args, Path(log_dir), int(index))))
        return 0

    log_dir = args.out / time.strftime("%Y%m%d-%H%M%S")
    log_dir.mkdir(parents=True)
    results = []
    for pair in [p for p in PAIRS if not args.pairs or p in args.pairs]:
        runs = []
        for index in range(args.repeat):
            # Each run in namespaces of its own, made afresh: no NAT remembers
            # the run before.
            outcome = subprocess.run(
                ["unshare", "--user", "--map-root-user", "--net", "--mount", "--propagation",
                 "private", sys.executable, __file__, *sys.argv[1:], "--inside", pair,
                 str(index), str(log_dir)],
                stdout=subprocess.PIPE, stderr=sys.stderr, text=True)
            lines = outcome.stdout.strip().splitlines()
            run = json.loads(lines[-1]) if outcome.returncode == 0 and lines else {}
            runs.append(run)
            got = sum(1 for v in run.get("viewers", []) if v.get("pictures", 0) > 0)
            print(f"{pair} #{index + 1}: {got} of {args.viewers} got pictures, the sender "
                  f"sending {cell(run.get('uplink_mbps'), 1)} Mbit/s", file=sys.stderr)
        results.append((pair, runs))
    print(table(results))
    print(f"\nlogs: {log_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
