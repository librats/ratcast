import { motion, useScroll, useTransform } from "motion/react";
import { ArrowRight, Lock, UserPlus, Zap } from "lucide-react";
import { useRef } from "react";
import { REPO_URL, useLatestRelease } from "../lib/release";
import { AppMock } from "./AppMock";
import { DownloadButton } from "./DownloadButton";
import { GitHubIcon } from "./ui";

const EASE = [0.22, 1, 0.36, 1] as const;

function Words({ text, delay }: { text: string; delay: number }) {
  return (
    <span>
      {text.split(" ").map((word, i) => (
        <motion.span
          key={i}
          className="inline-block whitespace-pre"
          initial={{ opacity: 0, y: "0.6em", filter: "blur(10px)" }}
          animate={{ opacity: 1, y: 0, filter: "blur(0px)" }}
          transition={{ duration: 0.8, delay: delay + i * 0.08, ease: EASE }}
        >
          {word}{" "}
        </motion.span>
      ))}
    </span>
  );
}

function Badge({ icon: Icon, children, className }: { icon: typeof Lock; children: string; className: string }) {
  return (
    <div
      className={`absolute z-10 hidden animate-float items-center gap-2 rounded-xl border border-white/10 bg-ink/70 px-3 py-2 text-xs shadow-xl backdrop-blur-xl xl:flex ${className}`}
    >
      <Icon className="size-4 text-cyan" />
      {children}
    </div>
  );
}

export function Hero() {
  const release = useLatestRelease();
  const stage = useRef<HTMLDivElement>(null);
  const { scrollYProgress } = useScroll({ target: stage, offset: ["start end", "center center"] });
  const tilt = useTransform(scrollYProgress, [0, 1], [24, 0]);
  const scale = useTransform(scrollYProgress, [0, 1], [0.92, 1]);

  return (
    <section id="top" className="relative isolate overflow-hidden pt-36 pb-24 md:pt-44">
      {/* aurora */}
      <div aria-hidden className="absolute inset-0 -z-10">
        <div className="absolute top-[-20%] left-[10%] size-[42rem] animate-aurora rounded-full bg-violet/25 blur-[120px]" />
        <div className="absolute top-[0%] right-[0%] size-[36rem] animate-aurora rounded-full bg-cyan/15 blur-[120px] [animation-delay:-6s]" />
        <div className="absolute top-[40%] left-[35%] size-[30rem] animate-aurora rounded-full bg-mint/10 blur-[120px] [animation-delay:-12s]" />
        <div className="bg-grid absolute inset-0" />
      </div>

      <div className="mx-auto max-w-6xl px-4 text-center">
        <motion.a
          href={release ? release.url : `${REPO_URL}/releases`}
          initial={{ opacity: 0, y: 10 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ duration: 0.6, ease: EASE }}
          className="group inline-flex items-center gap-2 rounded-full border border-white/10 bg-white/5 py-1 pr-3 pl-1 text-xs text-fog/70 backdrop-blur transition hover:border-white/20"
        >
          <span className="rounded-full bg-gradient-to-r from-violet to-cyan px-2 py-0.5 font-medium text-white">
            {release ? `v${release.version}` : "New"}
          </span>
          Free, open source, no account
          <ArrowRight className="size-3.5 transition group-hover:translate-x-0.5" />
        </motion.a>

        <h1 className="mx-auto mt-8 max-w-4xl text-5xl leading-[1.05] font-semibold tracking-tighter sm:text-6xl md:text-7xl lg:text-8xl">
          <Words text="Watch together." delay={0.1} />
          <br />
          {/* one piece: background-clip text does not reach into words that animate on their own */}
          <motion.span
            className="text-gradient inline-block animate-gradient pb-2"
            initial={{ opacity: 0, y: "0.4em", filter: "blur(10px)" }}
            animate={{ opacity: 1, y: 0, filter: "blur(0px)" }}
            transition={{ duration: 0.9, delay: 0.35, ease: EASE }}
          >
            Peer to peer.
          </motion.span>
        </h1>

        <motion.p
          initial={{ opacity: 0, y: 16 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ duration: 0.8, delay: 0.6, ease: EASE }}
          className="mx-auto mt-7 max-w-2xl text-lg text-pretty text-fog/60 md:text-xl"
        >
          One person shares a movie, a game or a desktop, and the others watch it live while everyone talks. Make a
          room, send the link: the picture goes straight to each friend, encrypted end to end.
        </motion.p>

        <motion.div
          initial={{ opacity: 0, y: 16 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ duration: 0.8, delay: 0.75, ease: EASE }}
          className="mt-10 flex flex-col items-center justify-center gap-4 sm:flex-row"
        >
          <DownloadButton />
          <a
            href={REPO_URL}
            className="inline-flex items-center gap-2 rounded-2xl border border-white/10 bg-white/5 px-5 py-4 text-sm font-medium backdrop-blur transition hover:border-white/25 hover:bg-white/10"
          >
            <GitHubIcon className="size-4" />
            Source on GitHub
          </a>
        </motion.div>

        <motion.p
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          transition={{ duration: 1, delay: 1 }}
          className="mt-5 font-mono text-xs text-fog/40"
        >
          Windows 10/11 · Linux AppImage · macOS 14+ · Android 10+ · MIT licensed
        </motion.p>
      </div>

      <div ref={stage} className="relative mx-auto mt-20 max-w-5xl px-4 [perspective:1600px]">
        <motion.div style={{ rotateX: tilt, scale }} className="relative origin-top">
          {/* glow behind the window */}
          <div
            aria-hidden
            className="absolute -inset-px -z-10 rounded-2xl bg-gradient-to-r from-violet via-cyan to-mint opacity-60 blur-2xl"
          />
          <Badge icon={Lock} className="top-24 -left-40">
            Noise + SFrame, end to end
          </Badge>
          <Badge icon={Zap} className="top-1/3 -right-40 [animation-delay:-2s]">
            ~18 ms capture → screen
          </Badge>
          <Badge icon={UserPlus} className="bottom-28 -left-36 [animation-delay:-4s]">
            Leo joined by link
          </Badge>
          <AppMock />
        </motion.div>
      </div>
    </section>
  );
}
