import { motion } from "motion/react";
import { Check, Copy, Layers, Radio, Repeat, Shield, Timer } from "lucide-react";
import { useState, type ReactNode } from "react";
import { REPO_URL } from "../lib/release";
import { Reveal, SectionHeading } from "./ui";

const CODE = `rats_config_t config = {.protocol = RATCAST_PROTOCOL};
rats_t node = rats_create(&config);
ratcast_t media;
ratcast_handlers_t handlers = {.user = app,
                               .stream_announced = on_stream,
                               .frame_received = on_frame};
ratcast_attach(node, &handlers, NULL, &media);  /* before rats_start() */
rats_start(node);
rats_connect(node, "sender.example", 7700);

/* on_stream: subscribe; a feed in order, 200 ms behind */
ratcast_subscribe(media, peer_id, stream->id, 200000);`;

// Just enough C colouring for one snippet: comments, strings, numbers, calls, types.
function highlight(line: string): ReactNode[] {
  const out: ReactNode[] = [];
  const token = /(\/\*.*?\*\/)|("[^"]*")|(\b\d+\b)|(\b(?:rats|ratcast)_\w+_t\b)|(\b(?:rats|ratcast)_\w+(?=\())|(\bRATCAST_\w+|NULL\b)|(\.\w+(?= =))/g;
  let last = 0;
  for (const m of line.matchAll(token)) {
    if (m.index > last) out.push(line.slice(last, m.index));
    const cls = m[1]
      ? "text-fog/35 italic"
      : m[2]
        ? "text-mint"
        : m[3]
          ? "text-amber-300"
          : m[4]
            ? "text-cyan"
            : m[5]
              ? "text-violet-300"
              : m[6]
                ? "text-pink-300"
                : "text-sky-300";
    out.push(
      <span key={m.index} className={cls}>
        {m[0]}
      </span>,
    );
    last = m.index + m[0].length;
  }
  out.push(line.slice(last));
  return out;
}

function CopyButton({ text }: { text: string }) {
  const [done, setDone] = useState(false);
  return (
    <button
      onClick={() => {
        navigator.clipboard?.writeText(text).then(() => {
          setDone(true);
          setTimeout(() => setDone(false), 1500);
        });
      }}
      className="flex items-center gap-1.5 rounded-lg border border-white/10 px-2.5 py-1 text-xs text-fog/60 transition hover:border-white/25 hover:text-fog"
    >
      {done ? <Check className="size-3.5 text-mint" /> : <Copy className="size-3.5" />}
      {done ? "Copied" : "Copy"}
    </button>
  );
}

export function CodeWindow({ title, code, lang }: { title: string; code: string; lang: "c" | "sh" }) {
  return (
    <div className="overflow-hidden rounded-2xl border border-white/10 bg-[#0a0b10] shadow-2xl shadow-black/50">
      <div className="flex items-center justify-between border-b border-white/5 px-4 py-2.5">
        <div className="flex items-center gap-2">
          <span className="size-2.5 rounded-full bg-white/15" />
          <span className="size-2.5 rounded-full bg-white/15" />
          <span className="size-2.5 rounded-full bg-white/15" />
          <span className="ml-2 font-mono text-xs text-fog/40">{title}</span>
        </div>
        <CopyButton text={code} />
      </div>
      <pre className="overflow-x-auto p-5 font-mono text-[13px] leading-6">
        {code.split("\n").map((line, i) => (
          <motion.div
            key={i}
            initial={{ opacity: 0, x: -8 }}
            whileInView={{ opacity: 1, x: 0 }}
            viewport={{ once: true }}
            transition={{ duration: 0.35, delay: i * 0.05 }}
            className="flex"
          >
            <span className="mr-5 w-5 shrink-0 text-right text-fog/20 select-none">{i + 1}</span>
            <span className="text-fog/80">
              {lang === "c" ? (
                highlight(line)
              ) : line.startsWith("#") ? (
                <span className="text-fog/35">{line}</span>
              ) : (
                <>
                  <span className="text-mint select-none">$ </span>
                  {line}
                </>
              )}
            </span>
          </motion.div>
        ))}
      </pre>
    </div>
  );
}

const POINTS = [
  { icon: Timer, title: "Deadline-bound delivery", text: "Each frame arrives in time or is dropped. FEC and retransmission repair what they can before it." },
  { icon: Radio, title: "Rate control", text: "Follows librats' path measurements; several streams to one peer share its path." },
  { icon: Repeat, title: "One to many", text: "One sender feeds many viewers, directly or through a server." },
  { icon: Shield, title: "SFrame on every frame", text: "With a key for each sender, on top of the Noise-encrypted connection." },
  { icon: Layers, title: "C and C++ APIs", text: "Moves encoded frames only — bring your own codecs. Carries MPEG-TS feeds too." },
];

export function Developers() {
  return (
    <section id="developers" className="relative mx-auto max-w-6xl scroll-mt-24 px-4 py-24">
      <SectionHeading
        eyebrow="libratcast"
        title={
          <>
            The transport underneath, <span className="text-gradient animate-gradient">yours to build on</span>
          </>
        }
      >
        libratcast moves encoded frames between peers — a counterpart to SRT, librist and WebRTC's media, on top of{" "}
        <a href="https://github.com/librats/librats" className="text-fog underline decoration-cyan/50 underline-offset-4 hover:decoration-cyan">
          librats
        </a>
        . No codecs, no capture: just media over P2P.
      </SectionHeading>

      <div className="mt-16 grid items-start gap-10 lg:grid-cols-[1.25fr_1fr]">
        <Reveal className="min-w-0 space-y-4">
          <CodeWindow title="receiver.c" code={CODE} lang="c" />
          <CodeWindow
            title="try it on one machine"
            lang="sh"
            code={`ratcast-send --source pattern --audio tone\nratcast-recv rats://127.0.0.1:7700`}
          />
        </Reveal>
        <div className="space-y-3">
          {POINTS.map((p, i) => (
            <Reveal key={p.title} delay={i * 0.08}>
              <div className="group flex gap-4 rounded-2xl border border-transparent p-4 transition hover:border-white/8 hover:bg-white/[0.02]">
                <div className="grid size-10 shrink-0 place-items-center rounded-xl bg-gradient-to-br from-violet/30 to-cyan/10 ring-1 ring-white/10 transition group-hover:from-violet/50">
                  <p.icon className="size-5 text-fog" />
                </div>
                <div>
                  <h3 className="font-semibold">{p.title}</h3>
                  <p className="mt-1 text-sm text-fog/55">{p.text}</p>
                </div>
              </div>
            </Reveal>
          ))}
          <Reveal delay={0.4}>
            <a href={`${REPO_URL}#libratcast`} className="ml-4 inline-block text-sm text-cyan hover:underline">
              Read the docs on GitHub →
            </a>
          </Reveal>
        </div>
      </div>
    </section>
  );
}
