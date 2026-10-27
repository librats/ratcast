import { motion } from "motion/react";
import logo from "@repo/docs/logo-dark.png";
import { REPO_URL, RELEASES_URL } from "../lib/release";
import { DownloadButton } from "./DownloadButton";
import { Logo } from "./Nav";
import { GitHubIcon } from "./ui";

const LINKS = [
  { title: "Product", items: [["Features", "#features"], ["How it works", "#how"], ["Download", "#download"]] },
  { title: "Developers", items: [["libratcast", "#developers"], ["Benchmarks", `${REPO_URL}/blob/main/docs/bench.md`], ["Roadmap", `${REPO_URL}/blob/main/docs/PLAN.md`]] },
  { title: "Project", items: [["GitHub", REPO_URL], ["Releases", RELEASES_URL], ["librats", "https://github.com/librats/librats"]] },
] as const;

export function Footer() {
  return (
    <footer className="relative overflow-hidden border-t border-white/5">
      <div className="mx-auto max-w-6xl px-4 pt-24">
        <div className="grid items-center gap-10 md:grid-cols-2">
          <div>
            <h2 className="text-4xl font-semibold tracking-tight text-balance md:text-5xl">
              Tonight's movie, <span className="text-gradient animate-gradient">on everyone's screen.</span>
            </h2>
            <div className="mt-8">
              <DownloadButton />
            </div>
          </div>
          <motion.img
            src={logo}
            alt="ratcast: a rat with headphones streaming to friends"
            initial={{ opacity: 0, scale: 0.9, rotate: -3 }}
            whileInView={{ opacity: 1, scale: 1, rotate: 0 }}
            viewport={{ once: true }}
            transition={{ duration: 1, ease: [0.22, 1, 0.36, 1] }}
            className="w-full max-w-md justify-self-center drop-shadow-[0_0_60px_rgb(139_92_246/0.35)]"
          />
        </div>

        <div className="mt-20 grid gap-10 border-t border-white/5 py-12 sm:grid-cols-2 md:grid-cols-4">
          <div>
            <Logo />
            <p className="mt-4 max-w-xs text-sm text-fog/45">Peer-to-peer calls and screen sharing for watching things together.</p>
            <a href={REPO_URL} className="mt-5 inline-flex text-fog/50 transition hover:text-fog" aria-label="GitHub">
              <GitHubIcon />
            </a>
          </div>
          {LINKS.map((group) => (
            <div key={group.title}>
              <p className="text-sm font-semibold">{group.title}</p>
              <ul className="mt-4 space-y-2.5 text-sm text-fog/50">
                {group.items.map(([label, href]) => (
                  <li key={label}>
                    <a href={href} className="transition hover:text-fog">
                      {label}
                    </a>
                  </li>
                ))}
              </ul>
            </div>
          ))}
        </div>
        <div className="flex flex-col justify-between gap-2 border-t border-white/5 py-6 text-xs text-fog/35 sm:flex-row">
          <span suppressHydrationWarning>© {new Date().getFullYear()} ratcast contributors · MIT License</span>
          <span>Built on librats · no accounts, no tracking</span>
        </div>
      </div>
    </footer>
  );
}
