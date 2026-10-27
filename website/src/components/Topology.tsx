import { AnimatePresence, motion, useInView, useReducedMotion } from "motion/react";
import { Server, ShieldCheck, Users } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { Reveal, SectionHeading } from "./ui";

type Point = { x: number; y: number };

const SHARER: Point = { x: 90, y: 210 };
const SERVER: Point = { x: 400, y: 110 };
const VIEWERS: (Point & { name: string; note?: string })[] = [
  { name: "V1", x: 400, y: 320 },
  { name: "V2", x: 710, y: 40 },
  { name: "V3", x: 710, y: 120 },
  { name: "V4", x: 710, y: 200, note: "symmetric NAT" },
  { name: "V5", x: 710, y: 300 },
  { name: "V6", x: 710, y: 380 },
];

type Mode = "mesh" | "rearranged";

function curve(a: Point, b: Point) {
  const mid = (a.x + b.x) / 2;
  return `M${a.x} ${a.y} C${mid} ${a.y}, ${mid} ${b.y}, ${b.x} ${b.y}`;
}

// Which links carry the share: every viewer straight from the sharer, or the room as README's
// picture has it: one copy to the server for V2–V4, one to V1, which passes it on to V5 and V6.
function edges(mode: Mode): { id: string; from: Point; to: Point; upload: boolean }[] {
  const [v1, v2, v3, v4, v5, v6] = VIEWERS;
  if (mode === "mesh") return VIEWERS.map((v) => ({ id: `s-${v.name}`, from: SHARER, to: v, upload: true }));
  return [
    { id: "s-server", from: SHARER, to: SERVER, upload: true },
    { id: "s-V1", from: SHARER, to: v1, upload: true },
    ...[v2, v3, v4].map((v) => ({ id: `server-${v.name}`, from: SERVER, to: v, upload: false })),
    ...[v5, v6].map((v) => ({ id: `V1-${v.name}`, from: v1, to: v, upload: false })),
  ];
}

function Node({ at, label, note, kind }: { at: Point; label: string; note?: string; kind: "sharer" | "server" | "viewer" }) {
  const r = kind === "sharer" ? 34 : kind === "server" ? 28 : 20;
  const fill = kind === "sharer" ? "url(#node-sharer)" : kind === "server" ? "#151826" : "#10121a";
  return (
    <g>
      {kind === "sharer" && (
        <circle cx={at.x} cy={at.y} r={r + 10} fill="none" stroke="#8b5cf6" strokeOpacity=".35">
          <animate attributeName="r" values={`${r};${r + 22}`} dur="2.4s" repeatCount="indefinite" />
          <animate attributeName="stroke-opacity" values=".5;0" dur="2.4s" repeatCount="indefinite" />
        </circle>
      )}
      <circle cx={at.x} cy={at.y} r={r} fill={fill} stroke="#e5ecf2" strokeOpacity=".18" />
      <text x={at.x} y={at.y + 4} textAnchor="middle" className="fill-fog font-mono text-[11px] font-semibold">
        {label}
      </text>
      {note && (
        <text x={at.x} y={at.y + r + 14} textAnchor="middle" className="fill-fog/40 font-mono text-[9px]">
          {note}
        </text>
      )}
    </g>
  );
}

export function Topology() {
  const [mode, setMode] = useState<Mode>("mesh");
  const [touched, setTouched] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  const inView = useInView(box, { margin: "-100px" });
  // After hydration: the prerendered page cannot know the visitor's setting.
  const reduced = useReducedMotion();
  const [still, setStill] = useState(false);
  useEffect(() => setStill(reduced ?? false), [reduced]);

  // Show both shapes of the room in turn until the visitor picks one.
  useEffect(() => {
    if (touched || !inView) return;
    const id = setInterval(() => setMode((m) => (m === "mesh" ? "rearranged" : "mesh")), 4200);
    return () => clearInterval(id);
  }, [touched, inView]);

  const links = edges(mode);
  const copies = links.filter((l) => l.upload).length;

  return (
    <section id="network" className="relative scroll-mt-24 overflow-hidden py-24">
      <div aria-hidden className="absolute top-1/3 left-1/2 -z-10 size-[50rem] -translate-x-1/2 rounded-full bg-violet/10 blur-[140px]" />
      <div className="mx-auto max-w-6xl px-4">
        <SectionHeading eyebrow="Network" title="A room that rearranges itself">
          Every room starts as a mesh. When the sharer's uplink can't carry a copy for everyone, the share is rearranged
          one viewer at a time: through a server, through another viewer with spare uplink, or still directly.
        </SectionHeading>

        <Reveal className="mt-14">
          <div ref={box} className="rounded-3xl border border-white/8 bg-white/[0.02] p-4 backdrop-blur md:p-8">
            <div className="flex flex-wrap items-center justify-between gap-4">
              <div className="inline-flex rounded-xl border border-white/10 bg-black/30 p-1 text-sm">
                {(["mesh", "rearranged"] as const).map((m) => (
                  <button
                    key={m}
                    onClick={() => {
                      setTouched(true);
                      setMode(m);
                    }}
                    className={`relative rounded-lg px-4 py-1.5 transition-colors ${mode === m ? "text-ink" : "text-fog/60 hover:text-fog"}`}
                  >
                    {mode === m && (
                      <motion.span layoutId="mode-pill" className="absolute inset-0 rounded-lg bg-fog" transition={{ type: "spring", bounce: 0.2, duration: 0.5 }} />
                    )}
                    <span className="relative">{m === "mesh" ? "Mesh" : "Rearranged"}</span>
                  </button>
                ))}
              </div>
              <div className="font-mono text-sm text-fog/60">
                sharer uploads{" "}
                <AnimatePresence mode="popLayout">
                  <motion.span
                    key={copies}
                    initial={{ y: -14, opacity: 0 }}
                    animate={{ y: 0, opacity: 1 }}
                    exit={{ y: 14, opacity: 0 }}
                    className={`inline-block text-lg font-semibold ${copies > 2 ? "text-amber-300" : "text-mint"}`}
                  >
                    {copies}
                  </motion.span>
                </AnimatePresence>{" "}
                copies
              </div>
            </div>

            <svg viewBox="0 0 800 420" className="mx-auto mt-6 w-full max-w-3xl">
              <defs>
                <radialGradient id="node-sharer">
                  <stop offset="0" stopColor="#8b5cf6" />
                  <stop offset="1" stopColor="#4c1d95" />
                </radialGradient>
                <linearGradient id="edge" x1="0" x2="1">
                  <stop offset="0" stopColor="#8b5cf6" />
                  <stop offset="1" stopColor="#22d3ee" />
                </linearGradient>
              </defs>

              <AnimatePresence>
                {links.map((l, i) => {
                  const d = curve(l.from, l.to);
                  return (
                    <motion.g key={l.id} initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }} transition={{ duration: 0.4 }}>
                      <motion.path
                        id={`edge-${l.id}`}
                        d={d}
                        fill="none"
                        stroke="url(#edge)"
                        strokeWidth={l.upload ? 2 : 1.5}
                        strokeOpacity={l.upload ? 0.8 : 0.45}
                        initial={{ pathLength: 0 }}
                        animate={{ pathLength: 1 }}
                        transition={{ duration: 0.8, delay: 0.05 * i, ease: "easeInOut" }}
                      />
                      {!still &&
                        [0, 1].map((k) => (
                          <circle key={k} r="3.5" fill={l.upload ? "#e5ecf2" : "#22d3ee"}>
                            <animateMotion dur="1.6s" begin={`${0.8 * k + 0.1 * i}s`} repeatCount="indefinite">
                              <mpath href={`#edge-${l.id}`} />
                            </animateMotion>
                          </circle>
                        ))}
                    </motion.g>
                  );
                })}
              </AnimatePresence>

              <motion.g animate={{ opacity: mode === "mesh" ? 0.25 : 1 }} transition={{ duration: 0.5 }}>
                <Node at={SERVER} label="server" kind="server" note="ratcast-server · SFU" />
              </motion.g>
              <Node at={SHARER} label="Sharer" kind="sharer" />
              {VIEWERS.map((v) => (
                <Node key={v.name} at={v} label={v.name} note={v.note} kind="viewer" />
              ))}
            </svg>
          </div>
        </Reveal>

        <div className="mt-6 grid gap-4 md:grid-cols-3">
          {[
            { icon: Server, title: "Through a server", text: "ratcast-server forwards the share like an SFU, so one upload serves every viewer it takes." },
            { icon: Users, title: "Through another viewer", text: "A viewer with spare uplink passes the frames on as they came, without re-encoding." },
            { icon: ShieldCheck, title: "Sealed on every hop", text: "Frames stay SFrame-encrypted the whole way. A server is never given a key." },
          ].map((c, i) => (
            <Reveal key={c.title} delay={i * 0.1} className="rounded-2xl border border-white/8 bg-white/[0.02] p-5">
              <c.icon className="size-5 text-cyan" />
              <h3 className="mt-3 font-semibold">{c.title}</h3>
              <p className="mt-1.5 text-sm text-fog/55">{c.text}</p>
            </Reveal>
          ))}
        </div>
      </div>
    </section>
  );
}
