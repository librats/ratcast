#!/usr/bin/env python3
"""ratcast-send -> ratcast-recv, each behind a NAT of its own, meeting at a
ratcast-server between them.

Five network namespaces in a user namespace of our own, as bench/netem.py
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

    bench/nat.py                          # every pair of NATs, once
    bench/nat.py cone-symmetric --repeat 3

Each one-way hop through the internet is delayed --delay ms (default 10), so
the sides are 4 x that apart and the server half of it from each.
"""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ beside netem.py
sys.path.insert(0, str(Path(__file__).resolve().parent))
from netem import INTERRUPTIBLE, ROOT, cell, netns_dir, parse_summary, sh, stop, tool  # noqa: E402

SERVER_ADDRESS = "203.0.113.1"
NAT_ADDRESS = "203.0.113.{}"  # side i's NAT on the internet: .11, .12
HOST_ADDRESS = "10.{}.0.2"  # side i's host behind it, its NAT at 10.i.0.1
KINDS = ("cone", "symmetric")
PAIRS = [f"{a}-{b}" for i, a in enumerate(KINDS) for b in KINDS[i:]]


def in_ns(name, *command):
    return ["ip", "netns", "exec", name, *command]


def make_network(kinds, delay_ms):
    """The internet (this namespace: a bridge, the server on it), and for each
    side a NAT of `kinds[i]` and a host behind it."""
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
        sh(*in_ns(nat, "tc", "qdisc", "add", "dev", "wan", "root", "netem", "delay",
                  f"{delay_ms}ms"))
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
    """The server's last word: (connections relayed now, MB relayed so far)."""
    lines = [line for line in Path(log).read_text().splitlines() if line.startswith("peers ")]
    if not lines:
        return None, None
    words = lines[-1].replace(",", "").replace("(", "").split()
    return int(words[words.index("relaying") + 1]), float(words[words.index("MB") - 1])


def run_once(pair, args, log_dir, index):
    stem = log_dir / f"{pair}.{index}"
    with open(f"{stem}.server.log", "w") as server_log, \
            open(f"{stem}.send.log", "w+") as send_log:
        server = subprocess.Popen(
            [str(tool(args.bin, "ratcast-server")), "--bind", SERVER_ADDRESS, "--stats-every",
             "1", *(["--verbose"] if args.verbose else [])],
            stdout=server_log, stderr=subprocess.STDOUT, text=True, **INTERRUPTIBLE)
        sender = None
        try:
            sender = subprocess.Popen(
                in_ns("host0", str(tool(args.bin, "ratcast-send")), "--source", "pattern",
                      "--size", args.size, "--bitrate", str(args.bitrate), "--fps",
                      str(args.fps), "--announce", "--no-dht", "--server", SERVER_ADDRESS,
                      *(["--verbose"] if args.verbose else [])),
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
            viewer = subprocess.run(
                in_ns("host1", str(tool(args.bin, "ratcast-recv")), f"rats://{peer}",
                      "--headless", "--server", SERVER_ADDRESS, "--no-dht", "--duration",
                      str(args.duration), *(["--verbose"] if args.verbose else [])),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                timeout=args.duration + 30)
            Path(f"{stem}.recv.log").write_text(viewer.stdout)
            relaying, relayed_mb = server_stats(server_log.name)
        finally:
            if sender:
                stop(sender)
            stop(server)
    recv = parse_summary(viewer.stdout) or {}
    return {**recv, "server_relaying": relaying, "server_mb": relayed_mb}


def table(results):
    rows = ["| NATs | pictures | first picture, ms | way | fps | stalls | through the server, MB |",
            "|---|---|---|---|---|---|---|"]
    for pair, runs in results:
        for run in runs:
            got = run.get("pictures", 0) > 0
            way = ("–" if not got else "server" if run.get("relayed") else "straight")
            rows.append(f"| {pair} | {'yes' if got else 'no'} | "
                        f"{cell(run.get('first_picture_ms')) if got else '–'} | {way} | "
                        f"{cell(run.get('fps'), 1)} | {cell(run.get('stalls'))} | "
                        f"{cell(run.get('server_mb'), 1)} |")
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
    parser.add_argument("--verbose", action="store_true", help="the tools' library logs")
    # One run, in namespaces made for it: PAIR INDEX LOG_DIR (the script calls
    # itself so); its outcome, JSON, the last line out.
    parser.add_argument("--inside", nargs=3, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
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
        make_network(pair.split("-"), args.delay)
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
            print(f"{pair} #{index + 1}: pictures {cell(run.get('pictures'))}, "
                  f"{'through the server' if run.get('relayed') else 'straight'}",
                  file=sys.stderr)
        results.append((pair, runs))
    print(table(results))
    print(f"\nlogs: {log_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
