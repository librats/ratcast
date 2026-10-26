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
capture to decoded picture off every picture. With --audio it sends a stamped
tone too, which the viewer plays in real time to nobody, and a second table
says how the sound fared: from capture to the speaker, what was lost, brought
back by FEC or made up, how long the jitter buffer held it, and how far apart
the pictures played from it (lip sync).

With --feed it measures transports of MPEG-TS feeds instead, ratcast's data
stream next to SRT and librist (their own tools, built by bench/peers.sh): each
carries the same stamped constant-bitrate probe, sent into it on the sender's
side and measured where it comes out on the viewer's (ratcast-tsprobe). Every
transport holds the feed for --latency to repair it in; the table says how
late it came, what was lost anyway, what crossed the link and the CPU it took.

    bench/netem.py                            # every scenario, once
    bench/netem.py --repeat 3 rtt-100 loss-2  # some of them, three times each
    bench/netem.py --audio --keyframe-every 2 local cap-20   # sound next to keyframes
    bench/netem.py --feed loss-2 burst        # ratcast, SRT and librist
    bench/netem.py --feed --transport ratcast,srt local
    bench/netem.py --list

With --room N it measures a room instead: N members (ratcast-room), each in a
namespace of its own on a bridge, everyone talking -- a stamped tone each --
and, with --share, the first sharing the stamped pattern. Each joins through
the one before it and meets the rest through them. Each member's uplink is the
scenario's forward direction (its backward one is not used), so a path between
two members is one scenario long, and a cap is what each may send to all the
others together. The table says whether everyone heard everyone, how soon the
last to join heard them all, how each voice fared -- the worst member's --
how the share played to the others, and what each member sent.

    bench/netem.py --room 4 local loss-2 cap-6
    bench/netem.py --room 3 --share --size 1280x720 --bitrate 3000 --fps 30 rtt-100

With --member-paths each member from the second on has an access path of its
own as well: its downlink that scenario's forward direction, its uplink the
backward one, in place of the run's. With --share the table then says how late
each viewer showed the pictures, and how far apart the viewers were.

    bench/netem.py --room 4 --share --member-paths local,rtt-100,loss-2 local

With --cut-off S@A the last member's network goes for S seconds, A seconds
after everyone is in, neither side told: a link silent past librats' idle
timeout (45 s) is lost and dialed again. The table then gives the longest
any voice was not heard: the cut, and how long the room took to mend.

    bench/netem.py --room 3 --duration 90 --cut-off 60@8 local

With --netsim the same scenarios run through ratcast-netsim instead: a proxy
on loopback whose two directions follow netem's rules in user space, with no
namespaces, no tc, no root, on any OS. The viewer dials the proxy, which
forwards to the sender. Its timers wake as late as the host's do: on a virtual
machine that sleeps, up to ten milliseconds.

    bench/netem.py --netsim --feed --transport ratcast loss-2

Prints a Markdown table (medians over the repeats); the tools' own output for
every run is kept in --out.
"""

import argparse
import json
import os
import re
import signal
import statistics
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SENDER_ADDRESS = "10.77.0.1"
VIEWER_ADDRESS = "10.77.0.2"
PORT = 7700
# Where a feed goes into the transport, on the sender's side, and comes out of
# it, on the viewer's.
FEED_IN = "udp://127.0.0.1:5000"
FEED_OUT = "udp://127.0.0.1:5001"
TRANSPORTS = ("ratcast", "srt", "rist")
# With --netsim: everything on loopback, the viewer dialing the proxy.
LOOPBACK = "127.0.0.1"
NETSIM_PORT = 7710
# With --room: member i in namespace m<i> at this address, on bridge "room".
ROOM_ADDRESS = "10.77.1.{}"


@dataclass
class Scenario:
    name: str
    path: str  # what it stands for, for the table
    forward: str = ""  # netem options, sender -> viewer
    backward: str = ""  # netem options, viewer -> sender
    # (seconds after the viewer starts, new forward options): a path that changes
    changes: tuple = ()


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
    Scenario("cap-drop", "50 ms RTT, 20 Mbit/s, 4 Mbit/s from 5 s to 10 s", capped(20, 25),
             "delay 25ms", changes=((5, capped(4, 25)), (10, capped(20, 25)))),
]


def sh(*command, check=True):
    return subprocess.run(command, check=check, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True)


def in_viewer(*command):
    return ["ip", "netns", "exec", "viewer", *command]


def netns_dir():
    """Where `ip netns` keeps its names, /run/netns, made ours."""
    # It may not be ours to create: then /run itself is replaced, in our mount
    # namespace only.
    try:
        Path("/run/netns").mkdir(exist_ok=True)
    except PermissionError:
        sh("mount", "-t", "tmpfs", "none", "/run")
        Path("/run/netns").mkdir()
    sh("mount", "-t", "tmpfs", "none", "/run/netns")


def mac(*show):
    return re.search(r"link/ether (\S+)", sh(*show).stdout).group(1)


def make_network():
    """Two namespaces joined by veth: this one (the sender's) and 'viewer'."""
    netns_dir()
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
    sh("ip", "neigh", "replace", VIEWER_ADDRESS, "lladdr",
       mac(*in_viewer("ip", "-o", "link", "show", "vb")), "dev", "va", "nud", "permanent")
    sh(*in_viewer("ip", "neigh", "replace", SENDER_ADDRESS, "lladdr",
                  mac("ip", "-o", "link", "show", "va"), "dev", "vb", "nud", "permanent"))


def in_member(index, *command):
    return ["ip", "netns", "exec", f"m{index}", *command]


def make_room_network(members):
    """A namespace for each member, m0, m1, ..., each on a veth to one bridge
    in this one; every member knows every other's MAC address from the start."""
    netns_dir()
    sh("ip", "link", "add", "room", "type", "bridge")
    sh("ip", "link", "set", "room", "up")
    for i in range(members):
        sh("ip", "netns", "add", f"m{i}")
        sh("ip", "link", "add", f"r{i}", "type", "veth", "peer", "name", "eth0", "netns", f"m{i}")
        sh("ip", "link", "set", f"r{i}", "master", "room", "up")
        sh(*in_member(i, "ip", "addr", "add", f"{ROOM_ADDRESS.format(i + 1)}/24", "dev", "eth0"))
        sh(*in_member(i, "ip", "link", "set", "eth0", "up"))
        sh(*in_member(i, "ip", "link", "set", "lo", "up"))
    macs = [mac(*in_member(i, "ip", "-o", "link", "show", "eth0")) for i in range(members)]
    for i in range(members):
        for j in range(members):
            if i != j:
                sh(*in_member(i, "ip", "neigh", "replace", ROOM_ADDRESS.format(j + 1), "lladdr",
                              macs[j], "dev", "eth0", "nud", "permanent"))


def shape_uplinks(members, options, member_paths=()):
    """Each member's uplink shaped by `options` (netem's), or left alone; a
    member with a path of its own (member_paths[i - 1]), both ways by that."""
    def shape(command, device, options):
        sh(*command("tc", "qdisc", "del", "dev", device, "root"), check=False)
        if options:
            sh(*command("tc", "qdisc", "add", "dev", device, "root", "netem", *options.split()))

    def here(*command):
        return list(command)

    for i in range(members):
        own = member_paths[i - 1] if 0 < i <= len(member_paths) else None
        # The bridge's side of the member's veth: what comes down to it.
        shape(here, f"r{i}", own.forward if own else "")
        shape(lambda *command: in_member(i, *command), "eth0", own.backward if own else options)


def uplink_bytes(index):
    """Bytes member `index` has sent so far."""
    show = in_member(index, "ip", "-j", "-s", "link", "show", "dev", "eth0")
    return json.loads(sh(*show).stdout)[0]["stats64"]["tx"]["bytes"]


def cut_off(index, dead, members, options, member_paths):
    """Member `index`'s network gone both ways, or back to its path."""
    if not dead:
        shape_uplinks(members, options, member_paths)
        return
    for command in (in_member(index, "tc", "qdisc", "replace", "dev", "eth0"),
                    ["tc", "qdisc", "replace", "dev", f"r{index}"]):
        sh(*command, "root", "netem", "loss", "100%")


def shape(scenario):
    for device, options, wrap in (("va", scenario.forward, lambda c: c),
                                  ("vb", scenario.backward, lambda c: in_viewer(*c))):
        sh(*wrap(["tc", "qdisc", "del", "dev", device, "root"]), check=False)
        if options:
            sh(*wrap(["tc", "qdisc", "add", "dev", device, "root", "netem", *options.split()]))


def shape_forward(options):
    sh("tc", "qdisc", "replace", "dev", "va", "root", "netem", *options.split())


class Netem:
    """The path as tc netem shapes the veth pair between the two namespaces
    (make_network): the sender in this one, the viewer in the other."""

    sender = SENDER_ADDRESS

    def __init__(self, scenario):
        self.scenario = scenario
        self.changes = []
        shape(scenario)

    def dial(self):
        return f"{SENDER_ADDRESS}:{PORT}"

    def viewer(self, command):
        return in_viewer(*command)

    def viewer_started(self):
        """The path's changes run from here."""
        self.changes = [threading.Timer(at, shape_forward, (options,))
                        for at, options in self.scenario.changes]
        for change in self.changes:
            change.start()

    def link_bytes(self):
        """Bytes the sender's end of the link has sent and received so far."""
        stats = json.loads(sh("ip", "-j", "-s", "link", "show", "dev", "va").stdout)[0]["stats64"]
        return stats["tx"]["bytes"], stats["rx"]["bytes"]

    def close(self):
        for change in self.changes:
            change.join()


class Netsim:
    """The path as ratcast-netsim emulates it on loopback: the viewer dials
    the proxy, which forwards to the sender."""

    sender = LOOPBACK

    def __init__(self, scenario, args, stem):
        changes = "; ".join(f"{at} {options}" for at, options in scenario.changes)
        self.log = open(f"{stem}.netsim.log", "w+")
        self.process = subprocess.Popen(
            [str(tool(args.bin, "ratcast-netsim")), "--listen", f"{LOOPBACK}:{NETSIM_PORT}", "--to",
             f"{LOOPBACK}:{PORT}", "--forward", scenario.forward, "--backward", scenario.backward,
             *(["--changes", changes] if changes else [])],
            stdout=self.log, stderr=subprocess.STDOUT, text=True, **INTERRUPTIBLE)
        deadline = time.monotonic() + 5
        while "netsim: dial" not in Path(self.log.name).read_text():
            if self.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError(f"ratcast-netsim did not start, see {self.log.name}")
            time.sleep(0.02)
        self.summary = {}

    def dial(self):
        return f"{LOOPBACK}:{NETSIM_PORT}"

    def viewer(self, command):
        return command

    def viewer_started(self):
        pass  # the proxy runs the path's changes from the viewer's first datagram

    def link_bytes(self):
        # Nothing crosses the proxy before the viewer dials, so its totals at
        # the end are the run's.
        return 0, 0

    def close(self):
        stop(self.process)
        self.log.close()
        self.summary = parse_summary(Path(self.log.name).read_text()) or {}
        return self.summary.get("forward_wire_bytes", 0), self.summary.get("backward_wire_bytes", 0)


def open_path(scenario, args, stem):
    return Netsim(scenario, args, stem) if args.netsim else Netem(scenario)


def parse_summary(text):
    for line in text.splitlines():
        if line.startswith("summary "):
            return {k: float(v) for k, v in re.findall(r"(\w+)=(-?[\d.]+)", line)}
    return None


def run_once(scenario, args, log_dir, index):
    stem = log_dir / f"{scenario.name}.{index}"
    path = open_path(scenario, args, stem)
    try:
        with open(f"{stem}.send.log", "w+") as send_log:
            sender = subprocess.Popen(
                [str(tool(args.bin, "ratcast-send")), "--source", "pattern", "--size", args.size,
                 "--bitrate", str(args.bitrate), "--fps", str(args.fps), "--latency",
                 str(args.latency), "--codec", args.codec, "--port", str(PORT),
                 *(["--audio", "tone"] if args.audio else []),
                 *(["--keyframe-every", str(args.keyframe_every)] if args.keyframe_every else []),
                 *args.send_args],
                stdout=send_log, stderr=subprocess.STDOUT, text=True, **INTERRUPTIBLE)
            try:
                deadline = time.monotonic() + 15
                while "viewers connect" not in Path(send_log.name).read_text():
                    if sender.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f"the sender did not start, see {send_log.name}")
                    time.sleep(0.05)
                # A viewer joins a stream that is already going, as in a real
                # call: not within the sender's first keyframe interval.
                time.sleep(args.join_after)
                path.viewer_started()
                viewer = subprocess.run(
                    path.viewer([str(tool(args.bin, "ratcast-recv")), "--connect", path.dial(),
                                 "--headless", "--duration", str(args.duration), *args.recv_args]),
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                    timeout=args.duration + 30)
                Path(f"{stem}.recv.log").write_text(viewer.stdout)
            finally:
                stop(sender)
    finally:
        path.close()
    return parse_summary(Path(f"{stem}.send.log").read_text()), parse_summary(viewer.stdout)


def tool(directory, name):
    """A tool's path: its name, and on Windows the .exe it has."""
    return directory / (f"{name}.exe" if os.name == "nt" else name)


# Processes stop() can interrupt: on Windows only Ctrl-Break reaches one, and
# only one in a process group of its own.
INTERRUPTIBLE = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {}


def stop(process):
    process.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def cpu_percent(process, started):
    """CPU the process has used since `started` (time.monotonic()), a share of
    one core; None where there is no /proc to read it from."""
    stat_file = Path(f"/proc/{process.pid}/stat")
    if not stat_file.exists():
        return None
    stat = stat_file.read_text().rsplit(")", 1)[1].split()
    ticks = int(stat[11]) + int(stat[12])  # utime and stime
    return 100 * ticks / os.sysconf("SC_CLK_TCK") / (time.monotonic() - started)


def feed_commands(transport, args, path):
    """The transport's sender (listening, as ratcast-send does) and receiver
    (connecting through `path`), each told to hold the feed for --latency."""
    latency = str(args.latency)
    dial = path.dial()
    if transport == "ratcast":
        return ([str(tool(args.bin, "ratcast-send")), "--input", FEED_IN, "--port", str(PORT),
                 "--latency", latency, *args.send_args],
                [str(tool(args.bin, "ratcast-recv")), f"rats://{dial}", "--output", FEED_OUT,
                 *args.recv_args])
    if transport == "srt":
        srt = str(args.peers / "srt-live-transmit")
        return ([srt, FEED_IN, f"srt://:{PORT}?mode=listener&latency={latency}", *args.send_args],
                [srt, f"srt://{dial}?latency={latency}", FEED_OUT, *args.recv_args])
    # librist's main profile, which SRT's live mode and ratcast compare with.
    return ([str(args.peers / "ristsender"), "--profile", "1", "--inputurl", FEED_IN,
             "--outputurl", f"rist://@{path.sender}:{PORT}?buffer={latency}", *args.send_args],
            [str(args.peers / "ristreceiver"), "--profile", "1", "--inputurl",
             f"rist://{dial}?buffer={latency}", "--outputurl", FEED_OUT, *args.recv_args])


def run_feed(scenario, transport, args, log_dir, index):
    """The probe through `transport`: the meter's summary, with what crossed
    the link (Mbit/s each way) and the CPU the transport's two ends took."""
    stem = log_dir / f"{scenario.name}.{transport}.{index}"
    path = open_path(scenario, args, stem)
    send_command, recv_command = feed_commands(transport, args, path)
    logs = {name: open(f"{stem}.{name}.log", "w") for name in ("send", "probe", "recv", "meter")}
    started = {}

    def start(name, command):
        started[name] = time.monotonic()
        return subprocess.Popen(command, stdout=logs[name], stderr=subprocess.STDOUT, text=True,
                                **INTERRUPTIBLE)

    processes = []
    try:
        sender = start("send", send_command)
        processes.append(sender)
        processes.append(start("probe", [str(tool(args.bin, "ratcast-tsprobe")), "send", FEED_IN,
                                         "--bitrate", str(args.bitrate)]))
        # The viewer joins a feed that is already going.
        time.sleep(args.join_after)
        if sender.poll() is not None:
            raise RuntimeError(f"the sender did not start, see {stem}.send.log")
        meter = start("meter", path.viewer([str(tool(args.bin, "ratcast-tsprobe")), "meter", FEED_OUT,
                                            "--duration", str(args.duration)]))
        processes.append(meter)
        receiver = start("recv", path.viewer(recv_command))
        processes.append(receiver)
        path.viewer_started()
        sent_before, received_before = path.link_bytes()
        meter.wait(timeout=args.duration + 30)
        took = time.monotonic() - started["recv"]
        sent, received = path.link_bytes()
        cpu = (cpu_percent(sender, started["send"]), cpu_percent(receiver, started["recv"]))
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                stop(process)
        for log in logs.values():
            log.close()
        totals = path.close()
    if totals:  # the proxy's, counted to its end
        sent, received = totals
    summary = parse_summary(Path(f"{stem}.meter.log").read_text()) or {}
    summary.update(wire_forward_mbps=(sent - sent_before) * 8 / took / 1e6,
                   wire_backward_mbps=(received - received_before) * 8 / took / 1e6)
    if cpu[0] is not None:
        summary.update(cpu_send=cpu[0], cpu_recv=cpu[1])
    return summary


def run_room(scenario, args, log_dir, index):
    """A room of args.room members, each joining through the one before, all
    leaving together: each member's summary, with what it sent (Mbit/s)."""
    stem = log_dir / f"{scenario.name}.{index}"
    members = args.room
    shape_uplinks(members, scenario.forward, args.member_paths)
    changes = [threading.Timer(at, shape_uplinks, (members, options, args.member_paths))
               for at, options in scenario.changes]
    logs = [open(f"{stem}.m{i}.log", "w+") for i in range(members)]
    processes = []
    try:
        for i in range(members):
            # Everyone leaves when the last has been in for --duration.
            stay = args.duration + (members - 1 - i) * args.join_after
            command = [str(tool(args.bin, "ratcast-room")), "--name", f"m{i}", "--port", str(PORT),
                       "--duration", f"{stay:.0f}"]
            if i > 0:
                command.append(f"rats://{ROOM_ADDRESS.format(i)}:{PORT}")
            if i == 0 and args.share:
                command += ["--share", "--size", args.size, "--bitrate", str(args.bitrate),
                            "--fps", str(args.fps), "--codec", args.codec]
            processes.append(subprocess.Popen(in_member(i, *command, *args.send_args),
                                              stdout=logs[i], stderr=subprocess.STDOUT, text=True))
            deadline = time.monotonic() + 15
            while "members join with" not in Path(logs[i].name).read_text():
                if processes[i].poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"member {i} did not start, see {logs[i].name}")
                time.sleep(0.05)
            if i == 0:
                for change in changes:
                    change.start()
                sent_before = [uplink_bytes(m) for m in range(members)]
                started = time.monotonic()
            if i + 1 < members:
                time.sleep(args.join_after)
        if args.cut_off:
            down, at = args.cut_off
            last = members - 1
            for delay, dead in ((at, True), (at + down, False)):
                cut = threading.Timer(delay, cut_off, (last, dead, members, scenario.forward,
                                                       args.member_paths))
                changes.append(cut)
                cut.start()
        for process in processes:
            process.wait(timeout=args.duration + members * args.join_after + 30)
        took = time.monotonic() - started
        sent = [uplink_bytes(m) for m in range(members)]
    finally:
        for change in changes:
            change.cancel()
        for process in processes:
            if process.poll() is None:
                stop(process)
        for log in logs:
            log.close()
    summaries = []
    for i in range(members):
        summary = parse_summary(Path(logs[i].name).read_text()) or {}
        summary["uplink_mbps"] = (sent[i] - sent_before[i]) * 8 / took / 1e6
        summaries.append(summary)
    return summaries


def room_run(summaries):
    """One run of a room, summed up: whether everyone met, the worst member's
    voices, the share as the others saw it, what the members sent."""
    def values(key, who=summaries):
        return [s[key] for s in who if key in s]

    def worst(key, who=summaries, pick=max):
        found = values(key, who)
        return pick(found) if found else None

    viewers = summaries[1:]
    return {
        "met": min(int(s.get("voices", 0)) for s in summaries),
        "joined_ms": summaries[-1].get("voices_ms"),
        **{key: worst(f"voice_latency_{key}_ms") for key in ("p50", "p95", "p99", "max")},
        "breaks": sum(values("voice_breaks")),
        "lost": sum(values("voice_lost")),
        "concealed_ms": sum(values("voice_concealed_ms")),
        "first_picture_ms": worst("first_picture_ms", viewers),
        "fps": worst("fps", viewers, min),
        "picture_p95": worst("latency_p95_ms", viewers),
        # Each viewer's, in order: how far apart they showed the pictures.
        "picture_p50s": [s.get("latency_p50_ms") for s in viewers],
        "together": summaries[1].get("together_ms") if viewers else None,
        "stalls": sum(values("stalls", viewers)) if values("stalls", viewers) else None,
        "uplink_first": summaries[0].get("uplink_mbps"),
        "uplink_others": worst("uplink_mbps", viewers),
        "cpu": worst("cpu"),
        "longest_gap_s": (gap / 1000 if (gap := worst("voice_longest_gap_ms")) is not None
                          else None),
    }


def room_table(results, args):
    members = args.room
    rows = [
        f"A room of {members}, each talking (a tone, Opus voice), each joining through the one "
        f"before; each member's uplink the scenario's forward direction"
        + (f"; the first shares {args.size} at {args.fps} fps, {args.codec}, {args.bitrate} "
           f"kbit/s at most" if args.share else "")
        + (f", members {' '.join(args.send_args)}" if args.send_args else "")
        + (f"; the last member's network gone for {args.cut_off[0]:g} s, {args.cut_off[1]:g} s "
           f"after everyone is in" if args.cut_off else "")
        + f"; {args.duration} s with everyone in, medians of {args.repeat}. Voices: the worst "
        f"member's; breaks, losses and concealment: all members' together.",
        "",
        "| scenario | path | voices heard | all heard after, ms | voice latency p50 / p95 / p99 "
        "/ max, ms | breaks | lost / concealed, ms "
        + ("| share: first picture, ms | fps | latency p95, ms | stalls " if args.share else "")
        + "| sent, Mbit/s: first / others | CPU, % |"
        + (" longest voice gap, s |" if args.cut_off else ""),
        "|---|---|---:|---:|---:|---:|---:|" + ("---:|---:|---:|---:|" if args.share else "")
        + "---:|---:|" + ("---:|" if args.cut_off else ""),
    ]
    for scenario, runs in results:
        def m(key, digits=0):
            found = [r[key] for r in runs if r.get(key) is not None]
            return cell(statistics.median(found) if found else None, digits)

        met = min(r["met"] for r in runs)
        rows.append(
            f"| {scenario.name} | {scenario.path} | {met} of {members - 1} | {m('joined_ms')} "
            f"| {m('p50', 1)} / {m('p95', 1)} / {m('p99', 1)} / {m('max', 1)} | {m('breaks')} "
            f"| {m('lost')} / {m('concealed_ms')} "
            + (f"| {m('first_picture_ms')} | {m('fps')} | {m('picture_p95', 1)} | {m('stalls')} "
               if args.share else "")
            + f"| {m('uplink_first', 2)} / {m('uplink_others', 2)} | {m('cpu')} |"
            + (f" {m('longest_gap_s', 1)} |" if args.cut_off else ""))
    if args.share:
        rows += share_in_step_table(results, args)
    return "\n".join(rows)


def share_in_step_table(results, args):
    """How late each viewer showed the pictures (their p50), and how far
    apart the viewers were: the latest's less the earliest's."""
    viewers = args.room - 1
    paths = [p.name for p in args.member_paths] + [None] * viewers
    rows = [
        "",
        "Pictures, viewer by viewer"
        + (f" (their paths: {', '.join(p or 'the run' for p in paths[:viewers])})"
           if args.member_paths else "")
        + ": latency p50, ms; apart: the latest less the earliest; every viewer at: the delay "
        "the sharer said they all play at, from capture.",
        "",
        "| scenario | " + " | ".join(f"m{i + 1}" for i in range(viewers))
        + " | apart, ms | every viewer at, ms |",
        "|---|" + "---:|" * (viewers + 2),
    ]
    for scenario, runs in results:
        def middle(values, digits=1):
            found = [v for v in values if v is not None]
            return cell(statistics.median(found) if found else None, digits)

        def apart(run):
            found = [v for v in run["picture_p50s"] if v is not None]
            return max(found) - min(found) if len(found) > 1 else None

        rows.append(
            f"| {scenario.name} | "
            + " | ".join(middle(r["picture_p50s"][i] for r in runs) for i in range(viewers))
            + f" | {middle(apart(r) for r in runs)} | {middle((r['together'] for r in runs), 0)} |")
    return rows


def median(runs, side, key):
    values = [r[side][key] for r in runs if r[side] and key in r[side]]
    return statistics.median(values) if values else None


def cell(value, digits=0):
    return "–" if value is None else f"{value:.{digits}f}"


def table(results, args):
    rows = [
        f"{args.size} at {args.fps} fps, {args.codec}, {args.bitrate} kbit/s at most, frame "
        f"lifetime {args.latency} ms"
        + (f", a keyframe every {args.keyframe_every} s" if args.keyframe_every else "")
        + (", with sound (a tone, Opus voice)" if args.audio else "")
        + (f", sender {' '.join(args.send_args)}" if args.send_args else "")
        + (f", viewer {' '.join(args.recv_args)}" if args.recv_args else "")
        + (", the path through ratcast-netsim" if args.netsim else "")
        + f"; {args.duration} s a run, medians of {args.repeat}.",
        "",
        "| scenario | path | first picture, ms | fps | Mbit/s (cuts) | latency p50 / p95 / p99, ms "
        "| stalls (ms) | lost / dropped | expired / sent | keyframes | repair | CPU send / recv, % |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for scenario, runs in results:
        first = median(runs, 1, "first_picture_ms")
        fps = median(runs, 1, "fps")
        rows.append(
            f"| {scenario.name} | {scenario.path} | {cell(first)} | {cell(fps)} "
            f"| {cell(median(runs, 1, 'mbps'), 1)} ({cell(median(runs, 0, 'bitrate_cuts'))}) "
            f"| {cell(median(runs, 1, 'latency_p50_ms'), 1)} / "
            f"{cell(median(runs, 1, 'latency_p95_ms'), 1)} / "
            f"{cell(median(runs, 1, 'latency_p99_ms'), 1)} "
            f"| {cell(median(runs, 1, 'stalls'))} ({cell(median(runs, 1, 'stalled_ms'))}) "
            f"| {cell(median(runs, 1, 'lost'))} / {cell(median(runs, 1, 'dropped'))} "
            f"| {cell(median(runs, 0, 'live_expired'))} / {cell(median(runs, 0, 'live_sent'))} "
            f"| {cell(median(runs, 0, 'keyframes'))} "
            f"| {cell(100 * (median(runs, 0, 'repair_share') or 0), 1)}% "
            f"| {cell(median(runs, 0, 'cpu'))} / {cell(median(runs, 1, 'cpu'))} |")
    if args.audio:
        rows += [
            "",
            "| scenario | first sound, ms | latency p50 / p95 / p99 / max, ms | jitter buffer "
            "delay / target, ms | lost / late | FEC / concealed, ms | breaks | kbit/s "
            "| pictures apart p50 / p95 / max (mean), ms |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
        for scenario, runs in results:
            rows.append(
                f"| {scenario.name} | {cell(median(runs, 1, 'first_sound_ms'))} "
                f"| {cell(median(runs, 1, 'audio_latency_p50_ms'), 1)} / "
                f"{cell(median(runs, 1, 'audio_latency_p95_ms'), 1)} / "
                f"{cell(median(runs, 1, 'audio_latency_p99_ms'), 1)} / "
                f"{cell(median(runs, 1, 'audio_latency_max_ms'), 1)} "
                f"| {cell(median(runs, 1, 'audio_delay_ms'))} / "
                f"{cell(median(runs, 1, 'audio_target_ms'))} "
                f"| {cell(median(runs, 1, 'audio_lost'))} / {cell(median(runs, 1, 'audio_late'))} "
                f"| {cell(median(runs, 1, 'audio_recovered_ms'))} / "
                f"{cell(median(runs, 1, 'audio_concealed_ms'))} "
                f"| {cell(median(runs, 1, 'audio_breaks'))} "
                f"| {cell(median(runs, 0, 'audio_kbps'), 1)} "
                f"| {cell(median(runs, 1, 'av_apart_p50_ms'), 1)} / "
                f"{cell(median(runs, 1, 'av_apart_p95_ms'), 1)} / "
                f"{cell(median(runs, 1, 'av_apart_max_ms'), 1)} "
                f"({cell(median(runs, 1, 'av_apart_signed_mean_ms'), 1)}) |")
    return "\n".join(rows)


def feed_table(results, args):
    rows = [
        f"A stamped MPEG-TS probe at {args.bitrate} kbit/s, held {args.latency} ms by each "
        f"transport" + (f", sender {' '.join(args.send_args)}" if args.send_args else "")
        + (f", viewer {' '.join(args.recv_args)}" if args.recv_args else "")
        + (", the path through ratcast-netsim" if args.netsim else "")
        + f"; {args.duration} s a run, medians of {args.repeat}.",
        "",
        "| scenario | transport | first packet, ms | latency p50 / p99 / max, ms | lost, % "
        "| glitches | longest gap, ms | link → / ←, Mbit/s | CPU send / recv, % |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for scenario, transport, runs in results:
        def m(key):
            values = [r[key] for r in runs if key in r]
            return statistics.median(values) if values else None

        rows.append(
            f"| {scenario.name} | {transport} | {cell(m('first_packet_ms'))} "
            f"| {cell(m('latency_p50_ms'), 1)} / {cell(m('latency_p99_ms'), 1)} / "
            f"{cell(m('latency_max_ms'), 1)} | {cell(m('loss_pct'), 3)} "
            f"| {cell(m('discontinuities'))} | {cell(m('longest_gap_ms'))} "
            f"| {cell(m('wire_forward_mbps'), 2)} / {cell(m('wire_backward_mbps'), 2)} "
            f"| {cell(m('cpu_send'), 1)} / {cell(m('cpu_recv'), 1)} |")
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
    parser.add_argument("--audio", action="store_true", help="send a tone too, and measure it")
    parser.add_argument("--keyframe-every", type=int, default=0,
                        help="seconds between keyframes the sender makes unasked (0: none)")
    parser.add_argument("--send-arg", dest="send_args", action="append", default=[],
                        help="one more option for ratcast-send (repeatable)")
    parser.add_argument("--recv-arg", dest="recv_args", action="append", default=[],
                        help="one more option for ratcast-recv (repeatable)")
    parser.add_argument("--feed", action="store_true",
                        help="measure transports of an MPEG-TS feed instead")
    parser.add_argument("--transport", default=",".join(TRANSPORTS),
                        help=f"with --feed: which, comma-separated (default {','.join(TRANSPORTS)})")
    parser.add_argument("--peers", type=Path, default=ROOT / "build" / "peers" / "bin",
                        help="SRT's and librist's tools (bench/peers.sh builds them)")
    parser.add_argument("--netsim", action="store_true",
                        help="the path through ratcast-netsim on loopback, not tc netem")
    parser.add_argument("--room", type=int, default=0, metavar="N",
                        help="measure a room of N members instead (2 to 8)")
    parser.add_argument("--share", action="store_true",
                        help="with --room: the first member shares the pattern")
    parser.add_argument("--cut-off", default="", metavar="S@A",
                        help="with --room: the last member's network gone for S seconds, "
                             "A seconds after everyone is in")
    parser.add_argument("--member-paths", default="", metavar="S1,S2,...",
                        help="with --room: the second member's own path, the third's, ...")
    args = parser.parse_args()

    if args.list:
        for s in SCENARIOS:
            print(f"{s.name:10} {s.path}\n{'':10}   -> {s.forward or '-'}\n{'':10}   <- "
                  f"{s.backward or '-'}")
            for at, options in s.changes:
                print(f"{'':10}   -> from {at} s: {options}")
        return 0
    unknown = set(args.scenarios) - {s.name for s in SCENARIOS}
    if unknown:
        parser.error(f"no such scenario: {', '.join(sorted(unknown))} (see --list)")
    if args.room and not 2 <= args.room <= 8:
        parser.error("--room: 2 to 8 members")
    if args.room and (args.netsim or args.feed):
        parser.error("--room runs over netem's namespaces: no --netsim, no --feed")
    if args.share and not args.room:
        parser.error("--share is --room's")
    by_name = {s.name: s for s in SCENARIOS}
    member_paths = [p for p in args.member_paths.split(",") if p]
    if member_paths and not args.room:
        parser.error("--member-paths is --room's")
    if set(member_paths) - by_name.keys():
        parser.error(f"--member-paths: no such scenario: "
                     f"{', '.join(sorted(set(member_paths) - by_name.keys()))}")
    if len(member_paths) > max(args.room - 1, 0):
        parser.error("--member-paths: one for each member after the first, at most")
    args.member_paths = [by_name[p] for p in member_paths]
    if args.cut_off:
        if not args.room:
            parser.error("--cut-off is --room's")
        try:
            down, at = (float(part) for part in args.cut_off.split("@"))
        except ValueError:
            parser.error("--cut-off: S@A, seconds, as 60@8")
        if at + down >= args.duration:
            parser.error("--cut-off: over before the run is (--duration)")
        args.cut_off = (down, at)
    transports = args.transport.split(",")
    if not set(transports) <= set(TRANSPORTS):
        parser.error(f"--transport: of {', '.join(TRANSPORTS)}, please")
    needed = (["ratcast-room"] if args.room else
              ["ratcast-send", "ratcast-recv", *(["ratcast-tsprobe"] if args.feed else []),
               *(["ratcast-netsim"] if args.netsim else [])])
    for name in needed:
        if not tool(args.bin, name).exists():
            parser.error(f"no {name} in {args.bin}: build the release preset, or pass --bin")
    if args.feed:
        peers = {"srt": ["srt-live-transmit"], "rist": ["ristsender", "ristreceiver"]}
        for name in (t for transport in transports for t in peers.get(transport, [])):
            if not (args.peers / name).exists():
                parser.error(f"no {name} in {args.peers}: run bench/peers.sh, or pass --peers")

    if not args.netsim:
        # Into namespaces of our own: the process starts over inside them.
        if os.environ.get("RATCAST_NETEM_INSIDE") != "1":
            os.environ["RATCAST_NETEM_INSIDE"] = "1"
            command = ["unshare", "--user", "--map-root-user", "--net", "--mount",
                       "--propagation", "private", sys.executable, *sys.argv]
            os.execvp(command[0], command)
        if args.room:
            make_room_network(args.room)
        else:
            make_network()
    log_dir = args.out / time.strftime("%Y%m%d-%H%M%S")
    log_dir.mkdir(parents=True)
    chosen = [s for s in SCENARIOS if not args.scenarios or s.name in args.scenarios]
    if args.room:
        results = []
        for scenario in chosen:
            runs = []
            for index in range(args.repeat):
                run = room_run(run_room(scenario, args, log_dir, index))
                runs.append(run)
                print(f"{scenario.name} #{index + 1}: heard {run['met']} of {args.room - 1}, "
                      f"voice p95 {cell(run['p95'], 1)} ms, breaks {run['breaks']}",
                      file=sys.stderr)
            results.append((scenario, runs))
        print(room_table(results, args))
        print(f"\nlogs: {log_dir}", file=sys.stderr)
        return 0
    if args.feed:
        results = []
        for scenario in chosen:
            for transport in transports:
                runs = []
                for index in range(args.repeat):
                    run = run_feed(scenario, transport, args, log_dir, index)
                    runs.append(run)
                    print(f"{scenario.name} {transport} #{index + 1}: p99 "
                          f"{cell(run.get('latency_p99_ms'), 1)} ms, lost "
                          f"{cell(run.get('loss_pct'), 3)}%", file=sys.stderr)
                results.append((scenario, transport, runs))
        print(feed_table(results, args))
        print(f"\nlogs: {log_dir}", file=sys.stderr)
        return 0
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
