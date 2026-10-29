import { motion } from "motion/react";
import {
  AudioLines,
  Check,
  Copy,
  Cpu,
  Gauge,
  Link2,
  LockKeyhole,
  MessagesSquare,
  MonitorPlay,
  Network,
  RefreshCw,
  Video,
  type LucideIcon,
} from "lucide-react";
import { useEffect, useState, type ReactNode } from "react";
import { Reveal, SectionHeading, SpotlightCard } from "./ui";

function Title({ icon: Icon, title, children }: { icon: LucideIcon; title: string; children: ReactNode }) {
  return (
    <div>
      <div className="mb-4 grid size-10 place-items-center rounded-xl border border-white/10 bg-gradient-to-br from-white/10 to-transparent">
        <Icon className="size-5 text-cyan" />
      </div>
      <h3 className="text-lg font-semibold tracking-tight">{title}</h3>
      <p className="mt-2 text-sm leading-relaxed text-fog/55">{children}</p>
    </div>
  );
}

function useTick(ms: number) {
  const [n, setN] = useState(0);
  useEffect(() => {
    const id = setInterval(() => setN((v) => v + 1), ms);
    return () => clearInterval(id);
  }, [ms]);
  return n;
}

const LINK = "rats://9f2c7a41b0d3e6ed1";

function RoomLink() {
  const n = useTick(90);
  const cycle = LINK.length + 30;
  const shown = Math.min(n % cycle, LINK.length);
  const copied = n % cycle > LINK.length + 8;
  return (
    <div className="mt-6 flex items-center gap-2 rounded-xl border border-white/10 bg-black/30 p-2 pl-4 font-mono text-sm">
      <span className="flex-1 truncate">
        {LINK.slice(0, shown)}
        <span className="ml-px inline-block h-4 w-px translate-y-0.5 animate-blink bg-cyan" />
      </span>
      <span
        className={`flex items-center gap-1.5 rounded-lg px-3 py-1.5 text-xs transition-colors ${
          copied ? "bg-mint/15 text-mint" : "bg-white/5 text-fog/70"
        }`}
      >
        {copied ? <Check className="size-3.5" /> : <Copy className="size-3.5" />}
        {copied ? "Copied" : "Copy"}
      </span>
    </div>
  );
}

function Equalizer({ bars = 28, className = "" }: { bars?: number; className?: string }) {
  return (
    <div className={`flex h-16 items-end gap-1 ${className}`}>
      {Array.from({ length: bars }, (_, i) => (
        <motion.span
          key={i}
          className="flex-1 rounded-full bg-gradient-to-t from-violet to-cyan"
          animate={{ height: ["20%", `${30 + ((i * 37) % 70)}%`, "35%", `${20 + ((i * 53) % 80)}%`, "20%"] }}
          transition={{ duration: 1.6 + (i % 5) * 0.2, repeat: Infinity, ease: "easeInOut", delay: (i % 7) * 0.1 }}
        />
      ))}
    </div>
  );
}

const CODECS = ["AV1", "HEVC", "H.264"];
const GPUS = ["NVENC", "AMF", "Quick Sync"];

function Encoders() {
  const n = useTick(1100);
  const lit = n % (CODECS.length * GPUS.length);
  return (
    <div className="mt-6 grid grid-cols-3 gap-1.5 font-mono text-[11px]">
      {GPUS.flatMap((gpu, g) =>
        CODECS.map((codec, c) => {
          const on = g * CODECS.length + c === lit;
          return (
            <span
              key={gpu + codec}
              className={`rounded-lg border px-2 py-1.5 text-center transition-all duration-500 ${
                on ? "border-cyan/60 bg-cyan/10 text-cyan shadow-[0_0_20px_-4px] shadow-cyan/60" : "border-white/8 text-fog/40"
              }`}
            >
              {codec}
              <span className="block text-[9px] opacity-70">{gpu}</span>
            </span>
          );
        }),
      )}
    </div>
  );
}

const GLYPHS = "ABCDEF0123456789";

function Cipher() {
  const n = useTick(70);
  const plain = "movie night, 9 pm";
  const sealed = Math.floor(n / 4) % 2 === 1;
  const text = Array.from({ length: 22 }, (_, i) =>
    sealed || i >= plain.length ? GLYPHS[(i * 13 + n * 7 + ((i * i * n) % 11)) % GLYPHS.length] : plain[i],
  ).join("");
  return (
    <div className="mt-6 overflow-hidden rounded-xl border border-white/10 bg-black/30 px-4 py-3 font-mono text-sm">
      <span className="mr-2 text-fog/30">frame</span>
      <span className={sealed ? "text-violet" : "text-fog/80"}>{text}</span>
    </div>
  );
}

function LatencyChart() {
  const points = Array.from({ length: 40 }, (_, i) => 30 + Math.sin(i * 0.7) * 6 + ((i * 17) % 7));
  const path = points.map((y, i) => `${i === 0 ? "M" : "L"}${(i / (points.length - 1)) * 400} ${y}`).join(" ");
  return (
    <svg viewBox="0 0 400 70" className="mt-6 w-full overflow-visible">
      <defs>
        <linearGradient id="lat-line" x1="0" x2="1">
          <stop offset="0" stopColor="#8b5cf6" />
          <stop offset="1" stopColor="#34d399" />
        </linearGradient>
        <linearGradient id="lat-fill" x1="0" x2="0" y1="0" y2="1">
          <stop offset="0" stopColor="#22d3ee" stopOpacity=".25" />
          <stop offset="1" stopColor="#22d3ee" stopOpacity="0" />
        </linearGradient>
      </defs>
      <line x1="0" x2="400" y1="18" y2="18" stroke="#e5ecf2" strokeOpacity=".15" strokeDasharray="4 6" />
      <text x="400" y="12" textAnchor="end" className="fill-fog/40 font-mono text-[9px]">
        frame deadline
      </text>
      <motion.path
        d={`${path} L400 70 L0 70 Z`}
        fill="url(#lat-fill)"
        initial={{ opacity: 0 }}
        whileInView={{ opacity: 1 }}
        viewport={{ once: true }}
        transition={{ duration: 1.2, delay: 0.6 }}
      />
      <motion.path
        d={path}
        fill="none"
        stroke="url(#lat-line)"
        strokeWidth="2.5"
        strokeLinecap="round"
        initial={{ pathLength: 0 }}
        whileInView={{ pathLength: 1 }}
        viewport={{ once: true }}
        transition={{ duration: 1.8, ease: "easeInOut" }}
      />
    </svg>
  );
}

const ROUTES = ["Port mapping", "Hole punching", "Relay via server"];

function NatSteps() {
  const n = useTick(1200);
  return (
    <div className="mt-6 space-y-2">
      {ROUTES.map((r, i) => (
        <div
          key={r}
          className={`flex items-center gap-3 rounded-lg border px-3 py-2 text-xs transition-all duration-500 ${
            n % ROUTES.length === i ? "border-mint/50 bg-mint/10 text-mint" : "border-white/8 text-fog/40"
          }`}
        >
          <span className="font-mono">{i + 1}</span>
          {r}
        </div>
      ))}
    </div>
  );
}

function CameraGrid() {
  const n = useTick(1400);
  return (
    <div className="mt-6 grid grid-cols-3 gap-1.5">
      {Array.from({ length: 6 }, (_, i) => (
        <div
          key={i}
          className={`aspect-[4/3] rounded-lg border-2 bg-gradient-to-br from-white/8 to-transparent transition-colors duration-300 ${
            n % 6 === i ? "border-mint" : "border-white/5"
          }`}
        />
      ))}
    </div>
  );
}

const MESSAGES = [
  { who: "Mia", text: "who brought snacks" },
  { who: "Leo", text: "github.com/librats 🔗" },
  { who: "Sam", text: "joining in 2 min" },
];

function ChatFeed() {
  const n = useTick(1600);
  // Numbered as they come, so each tick the oldest goes and the others slide up for a new one.
  const visible = [0, 1, 2].map((k) => ({ id: n + k, ...MESSAGES[(n + k) % MESSAGES.length] }));
  return (
    <div className="mt-6 space-y-1.5 text-xs">
      {visible.map((m) => (
        <motion.div
          key={m.id}
          layout
          initial={{ opacity: 0, y: 12 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ duration: 0.4 }}
          className="rounded-lg bg-white/5 px-3 py-1.5"
        >
          <span className="font-semibold text-cyan">{m.who}</span> <span className="text-fog/70">{m.text}</span>
        </motion.div>
      ))}
    </div>
  );
}

function UpdateBar() {
  return (
    <div className="mt-6">
      <div className="flex justify-between font-mono text-[11px] text-fog/40">
        <span>SHA-256 ✓</span>
        <span>never mid-call</span>
      </div>
      <div className="mt-2 h-2 overflow-hidden rounded-full bg-white/5">
        <motion.div
          className="h-full rounded-full bg-gradient-to-r from-violet via-cyan to-mint"
          animate={{ width: ["0%", "100%", "100%"] }}
          transition={{ duration: 4, repeat: Infinity, times: [0, 0.7, 1], ease: "easeInOut" }}
        />
      </div>
    </div>
  );
}

const CARDS: { span: string; icon: LucideIcon; title: string; text: string; demo: ReactNode }[] = [
  {
    span: "lg:col-span-3",
    icon: Link2,
    title: "Rooms by link",
    text: "Make a room, copy its rats:// link and send it to whoever should join. Peers find each other through the DHT, so a server is optional.",
    demo: <RoomLink />,
  },
  {
    span: "lg:col-span-3",
    icon: MonitorPlay,
    title: "A screen or a window, with its sound",
    text: "Viewers hear what your applications play, but never the call's own voices. Games, movies, a whole desktop.",
    demo: <Equalizer className="mt-6" />,
  },
  {
    span: "lg:col-span-2",
    icon: AudioLines,
    title: "Voice that is easy to listen to",
    text: "Echo cancellation, noise suppression and automatic gain from WebRTC AudioProcessing. Mute, deafen, see who speaks.",
    demo: <Equalizer bars={18} className="mt-6 h-10 opacity-80" />,
  },
  {
    span: "lg:col-span-2",
    icon: Cpu,
    title: "Hardware encoding",
    text: "AV1, HEVC and H.264 on NVENC, AMF or Quick Sync; HEVC and H.264 on VideoToolbox on a Mac. On Windows and macOS the screen reaches the encoder without leaving the GPU.",
    demo: <Encoders />,
  },
  {
    span: "lg:col-span-2",
    icon: LockKeyhole,
    title: "Encrypted end to end",
    text: "Connections use the Noise protocol and every frame is sealed again with SFrame. A server that forwards it can't read it.",
    demo: <Cipher />,
  },
  {
    span: "lg:col-span-4",
    icon: Gauge,
    title: "Low latency on real networks",
    text: "Rate control follows the path, and FEC and retransmission repair losses within each frame's deadline. A frame is in time or dropped: the stream never stalls behind an old one.",
    demo: <LatencyChart />,
  },
  {
    span: "lg:col-span-2",
    icon: Network,
    title: "Gets through NATs",
    text: "Port mapping and hole punching first, a relay through ratcast-server when nothing else works.",
    demo: <NatSteps />,
  },
  {
    span: "lg:col-span-2",
    icon: Video,
    title: "Cameras",
    text: "Show yourself next to the share, or meet in a grid where whoever speaks is ringed.",
    demo: <CameraGrid />,
  },
  {
    span: "lg:col-span-2",
    icon: MessagesSquare,
    title: "Text chat",
    text: "Write to the room beside the call. Late joiners see the last hundred messages.",
    demo: <ChatFeed />,
  },
  {
    span: "lg:col-span-2",
    icon: RefreshCw,
    title: "Updates itself",
    text: "New releases install from GitHub, checked by SHA-256 — but never while a call is on.",
    demo: <UpdateBar />,
  },
];

export function Features() {
  return (
    <section id="features" className="relative mx-auto max-w-6xl scroll-mt-24 px-4 py-24">
      <SectionHeading
        eyebrow="Features"
        title={
          <>
            Everything a movie night needs. <span className="text-fog/40">Nothing in the middle.</span>
          </>
        }
      >
        A desktop app for calls and screen streams, built in C++ on its own peer-to-peer media transport.
      </SectionHeading>

      <div className="mt-16 grid grid-cols-1 gap-4 md:grid-cols-2 lg:grid-cols-6">
        {CARDS.map((c, i) => (
          <Reveal key={c.title} delay={(i % 3) * 0.08} className={c.span}>
            <SpotlightCard className="h-full">
              <Title icon={c.icon} title={c.title}>
                {c.text}
              </Title>
              {c.demo}
            </SpotlightCard>
          </Reveal>
        ))}
      </div>
    </section>
  );
}
