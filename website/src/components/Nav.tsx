import { motion, useMotionValueEvent, useScroll } from "motion/react";
import { Download, Star } from "lucide-react";
import { useState } from "react";
import icon from "@repo/app/resources/ratcast.png";
import { REPO_URL, useStars } from "../lib/release";
import { GitHubIcon } from "./ui";

const LINKS = [
  ["Features", "#features"],
  ["How it works", "#how"],
  ["Network", "#network"],
  ["Developers", "#developers"],
] as const;

export function Logo() {
  return (
    <a href="#top" className="flex items-center gap-2.5 font-semibold tracking-tight">
      <img src={icon} alt="" className="size-8 invert" />
      <span className="text-lg">ratcast</span>
    </a>
  );
}

export function Nav() {
  const stars = useStars();
  const { scrollY } = useScroll();
  const [scrolled, setScrolled] = useState(false);
  useMotionValueEvent(scrollY, "change", (y) => setScrolled(y > 24));

  return (
    <motion.header
      initial={{ y: -40, opacity: 0 }}
      animate={{ y: 0, opacity: 1 }}
      transition={{ duration: 0.6, ease: [0.22, 1, 0.36, 1] }}
      className="fixed inset-x-0 top-0 z-50 flex justify-center px-4 pt-4"
    >
      <nav
        className={`flex w-full max-w-6xl items-center justify-between gap-4 rounded-2xl border px-4 py-2.5 transition-all duration-500 ${
          scrolled ? "border-white/10 bg-ink/70 shadow-2xl shadow-black/40 backdrop-blur-xl" : "border-transparent"
        }`}
      >
        <Logo />
        <div className="hidden items-center gap-1 md:flex">
          {LINKS.map(([label, href]) => (
            <a
              key={href}
              href={href}
              className="rounded-lg px-3 py-1.5 text-sm text-fog/60 transition hover:bg-white/5 hover:text-fog"
            >
              {label}
            </a>
          ))}
        </div>
        <div className="flex items-center gap-2">
          <a
            href={REPO_URL}
            className="hidden items-center gap-2 rounded-xl border border-white/10 px-3 py-1.5 text-sm text-fog/80 transition hover:border-white/20 hover:text-fog sm:flex"
          >
            <GitHubIcon className="size-4" />
            {stars !== null && (
              <span className="flex items-center gap-1 font-mono text-xs">
                <Star className="size-3.5 fill-amber-300 text-amber-300" />
                {stars.toLocaleString("en")}
              </span>
            )}
            {stars === null && <span>GitHub</span>}
          </a>
          <a
            href="#download"
            className="flex items-center gap-2 rounded-xl bg-fog px-3.5 py-1.5 text-sm font-medium text-ink transition hover:bg-white"
          >
            <Download className="size-4" />
            Download
          </a>
        </div>
      </nav>
    </motion.header>
  );
}
