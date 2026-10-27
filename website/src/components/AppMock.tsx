import { AnimatePresence, motion } from "motion/react";
import { Camera, Headphones, Mic, MonitorUp, PhoneOff, Radio, type LucideIcon } from "lucide-react";
import { useEffect, useState } from "react";

const MEMBERS = [
  { name: "You", hue: "from-violet to-fuchsia-500" },
  { name: "Mia", hue: "from-cyan to-sky-600" },
  { name: "Leo", hue: "from-mint to-emerald-700" },
  { name: "Sam", hue: "from-amber-400 to-orange-600" },
];

const CHAT = ["that jump scare 😱", "volume up pls", "no lag at all?!", "rewind 10s 👀"];

/** A sunset over a running grid: what the sharer happens to be playing. */
function Scene() {
  return (
    <div className="absolute inset-0 overflow-hidden bg-gradient-to-b from-[#1a0b3d] via-[#5b1a6e] to-[#f0627a]">
      <div className="absolute top-[14%] left-1/2 size-[38%] -translate-x-1/2 rounded-full bg-gradient-to-b from-amber-200 via-orange-400 to-pink-500 shadow-[0_0_120px_30px] shadow-pink-500/50 [mask-image:repeating-linear-gradient(to_bottom,black_0_9%,transparent_9%_11%)]" />
      <div className="absolute inset-x-0 bottom-0 h-[44%] bg-[#12051f] [perspective:220px]">
        <div
          className="absolute inset-x-[-50%] top-0 h-[200%] origin-top animate-grid-run [transform:rotateX(62deg)]"
          style={{
            backgroundImage:
              "linear-gradient(to right, rgb(244 114 182 / .7) 1px, transparent 1px), linear-gradient(to bottom, rgb(244 114 182 / .7) 1px, transparent 1px)",
            backgroundSize: "40px 40px",
          }}
        />
        <div className="absolute inset-0 bg-gradient-to-b from-[#12051f] via-transparent to-transparent" />
      </div>
      {/* mountains */}
      <svg viewBox="0 0 400 60" preserveAspectRatio="none" className="absolute inset-x-0 bottom-[43%] h-[16%] w-full">
        <path d="M0 60 L40 30 L70 45 L120 12 L170 44 L210 26 L260 50 L300 18 L350 40 L400 22 L400 60 Z" fill="#12051f" />
      </svg>
    </div>
  );
}

function useTicker<T>(values: readonly T[], ms: number): T {
  const [i, setI] = useState(0);
  useEffect(() => {
    const id = setInterval(() => setI((n) => (n + 1) % values.length), ms);
    return () => clearInterval(id);
  }, [values, ms]);
  return values[i];
}

function useLatency() {
  const [ms, setMs] = useState(18);
  useEffect(() => {
    const id = setInterval(() => setMs(15 + Math.round(Math.random() * 6)), 900);
    return () => clearInterval(id);
  }, []);
  return ms;
}

const SPEAKERS = [1, 2, 1, 3, 0] as const;

const BUTTONS: { Icon: LucideIcon; label: string }[] = [
  { Icon: Mic, label: "Mute" },
  { Icon: Headphones, label: "Deafen" },
  { Icon: Camera, label: "Camera" },
  { Icon: MonitorUp, label: "Share" },
  { Icon: Radio, label: "Stats" },
];

/** The app in the middle of a movie night, drawn after its own window. */
export function AppMock() {
  const latency = useLatency();
  const speaker = useTicker(SPEAKERS, 1800);
  const message = useTicker(CHAT, 2600);

  return (
    <div className="overflow-hidden rounded-2xl border border-white/10 bg-[#1e1f22] shadow-2xl shadow-black/60">
      {/* title bar */}
      <div className="flex items-center gap-2 border-b border-white/5 bg-[#17181b] px-4 py-2.5">
        <span className="size-3 rounded-full bg-[#ff5f57]" />
        <span className="size-3 rounded-full bg-[#febc2e]" />
        <span className="size-3 rounded-full bg-[#28c840]" />
        <span className="ml-3 font-mono text-xs text-fog/40">ratcast — rats://9f2c…e6ed1</span>
      </div>

      <div className="grid grid-cols-1 gap-3 p-3 sm:grid-cols-[1fr_auto]">
        {/* the share */}
        <div className="relative aspect-video overflow-hidden rounded-xl">
          <Scene />
          <div className="absolute top-3 left-3 flex items-center gap-2">
            <span className="flex items-center gap-1.5 rounded-md bg-red-500/90 px-2 py-0.5 text-[10px] font-bold tracking-wider text-white">
              <span className="size-1.5 animate-blink rounded-full bg-white" />
              LIVE
            </span>
            <span className="hidden rounded-md bg-black/50 px-2 py-0.5 font-mono text-[10px] text-white/80 backdrop-blur sm:inline">
              1080p60 · AV1 · NVENC
            </span>
          </div>
          <div className="absolute top-3 right-3 rounded-md bg-black/50 px-2 py-0.5 font-mono text-[10px] text-mint backdrop-blur">
            <span className="tabular-nums">{latency}</span> ms
          </div>
          <div className="absolute bottom-3 left-3 max-w-[70%]">
            <AnimatePresence mode="wait">
              <motion.div
                key={message}
                initial={{ opacity: 0, y: 10, scale: 0.95 }}
                animate={{ opacity: 1, y: 0, scale: 1 }}
                exit={{ opacity: 0, y: -6 }}
                transition={{ duration: 0.35 }}
                className="rounded-xl rounded-bl-sm bg-black/55 px-3 py-1.5 text-xs text-white backdrop-blur"
              >
                <span className="font-semibold text-cyan">{MEMBERS[1 + (CHAT.indexOf(message) % 3)].name}</span>{" "}
                {message}
              </motion.div>
            </AnimatePresence>
          </div>
        </div>

        {/* cameras */}
        <div className="flex gap-3 sm:flex-col">
          {MEMBERS.map((m, i) => (
            <div
              key={m.name}
              className={`relative grid aspect-[4/3] flex-1 place-items-center rounded-xl border-2 bg-[#111214] transition-colors duration-300 sm:w-28 sm:flex-none ${
                speaker === i ? "border-mint" : "border-[#4e5058]/60"
              }`}
            >
              <div
                className={`grid size-9 place-items-center rounded-full bg-gradient-to-br text-sm font-semibold text-white ${m.hue} ${
                  speaker === i ? "animate-pulse-ring" : ""
                }`}
              >
                {m.name[0]}
              </div>
              <span className="absolute bottom-1 left-1.5 text-[10px] text-fog/60">{m.name}</span>
            </div>
          ))}
        </div>
      </div>

      {/* the app's own buttons */}
      <div className="flex flex-wrap items-center justify-center gap-2 border-t border-white/5 bg-[#17181b] px-3 py-3">
        {BUTTONS.map(({ Icon, label }) => (
          <span
            key={label}
            className="flex items-center gap-1.5 rounded-full bg-[#2b2d31] px-3 py-1.5 text-[11px] text-fog/80"
          >
            <Icon className="size-3.5" />
            <span className="hidden sm:inline">{label}</span>
          </span>
        ))}
        <span className="flex items-center gap-1.5 rounded-full bg-red-600 px-3 py-1.5 text-[11px] font-medium text-white">
          <PhoneOff className="size-3.5" />
          <span className="hidden sm:inline">Leave</span>
        </span>
      </div>
    </div>
  );
}
