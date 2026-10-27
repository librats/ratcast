import { animate, motion, useInView, useMotionValue, useTransform } from "motion/react";
import { useEffect, useRef } from "react";
import { Reveal } from "./ui";

const TECH = [
  "AV1",
  "HEVC",
  "H.264",
  "NVENC",
  "AMF",
  "Quick Sync",
  "SVT-AV1",
  "Opus",
  "WebRTC AudioProcessing",
  "Noise protocol",
  "SFrame",
  "DHT",
  "FEC",
  "Hole punching",
  "Direct3D 11",
  "PipeWire",
  "librats",
];

export function Marquee() {
  return (
    <div className="relative overflow-hidden border-y border-white/5 bg-white/[0.015] py-5 [mask-image:linear-gradient(to_right,transparent,black_15%,black_85%,transparent)]">
      <div className="flex w-max animate-marquee gap-10 hover:[animation-play-state:paused]">
        {[...TECH, ...TECH].map((t, i) => (
          <span key={i} className="flex items-center gap-10 font-mono text-sm whitespace-nowrap text-fog/40">
            {t}
            <span className="size-1 rounded-full bg-fog/20" />
          </span>
        ))}
      </div>
    </div>
  );
}

function Counter({ to, suffix = "", prefix = "" }: { to: number; suffix?: string; prefix?: string }) {
  const ref = useRef<HTMLSpanElement>(null);
  const inView = useInView(ref, { once: true });
  const value = useMotionValue(0);
  const text = useTransform(value, (v) => `${prefix}${Math.round(v)}${suffix}`);
  useEffect(() => {
    if (inView) animate(value, to, { duration: 1.6, ease: [0.22, 1, 0.36, 1] });
  }, [inView, to, value]);
  return <motion.span ref={ref}>{text}</motion.span>;
}

const STATS = [
  { value: <Counter prefix="~" to={18} suffix=" ms" />, label: "from capture to a friend's screen on one host" },
  { value: <Counter to={0} />, label: "accounts, sign-ups or servers you have to trust" },
  { value: <Counter to={2} suffix="×" />, label: "encrypted: Noise on the wire, SFrame on every frame" },
  { value: "MIT", label: "open source, no GPL anywhere in the stack" },
];

export function Stats() {
  return (
    <section className="mx-auto max-w-6xl px-4 py-24">
      <div className="grid grid-cols-2 gap-px overflow-hidden rounded-3xl border border-white/8 bg-white/8 md:grid-cols-4">
        {STATS.map((s, i) => (
          <Reveal key={i} delay={i * 0.08} className="bg-ink p-8">
            <div className="text-gradient animate-gradient text-4xl font-semibold tracking-tight tabular-nums md:text-5xl">
              {s.value}
            </div>
            <p className="mt-3 text-sm text-fog/50">{s.label}</p>
          </Reveal>
        ))}
      </div>
    </section>
  );
}
