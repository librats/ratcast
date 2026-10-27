import { motion } from "motion/react";
import { Link2, MonitorUp, Popcorn, type LucideIcon } from "lucide-react";
import { Reveal, SectionHeading } from "./ui";

const STEPS: { icon: LucideIcon; title: string; text: string; detail: string }[] = [
  {
    icon: Link2,
    title: "Make a room",
    text: "Press one button and the app gives you a link. Send it to your friends however you like.",
    detail: "rats://PEER-ID",
  },
  {
    icon: Popcorn,
    title: "Friends join",
    text: "They paste the link and press Join, or open it from the command line: ratcast rats://…",
    detail: "no account, no install wizard",
  },
  {
    icon: MonitorUp,
    title: "Share and watch",
    text: "Press Share, pick a screen or a window and whether its sound goes too. The others press Watch.",
    detail: "F full screen · Ctrl+I stats",
  },
];

export function HowItWorks() {
  return (
    <section id="how" className="relative mx-auto max-w-6xl scroll-mt-24 px-4 py-24">
      <SectionHeading eyebrow="How it works" title="From zero to watching in three steps">
        No sign-up, no server to set up, nothing to configure. Unzip, run, send a link.
      </SectionHeading>

      <div className="relative mt-20 grid gap-10 md:grid-cols-3 md:gap-6">
        {/* the line that joins the steps, drawn as it scrolls in */}
        <motion.div
          aria-hidden
          className="absolute top-8 left-[16.6%] hidden h-px w-[66.8%] origin-left bg-gradient-to-r from-violet via-cyan to-mint md:block"
          initial={{ scaleX: 0 }}
          whileInView={{ scaleX: 1 }}
          viewport={{ once: true, margin: "-120px" }}
          transition={{ duration: 1.4, ease: "easeInOut" }}
        />

        {STEPS.map((s, i) => (
          <Reveal key={s.title} delay={0.2 + i * 0.25} className="relative text-center">
            <div className="relative mx-auto grid size-16 place-items-center rounded-2xl border border-white/10 bg-ink shadow-[0_0_40px_-10px] shadow-violet/60">
              <s.icon className="size-7 text-fog" />
              <span className="absolute -top-2 -right-2 grid size-6 place-items-center rounded-full bg-gradient-to-br from-violet to-cyan font-mono text-xs font-bold text-white">
                {i + 1}
              </span>
            </div>
            <h3 className="mt-6 text-xl font-semibold tracking-tight">{s.title}</h3>
            <p className="mx-auto mt-3 max-w-xs text-sm leading-relaxed text-fog/55">{s.text}</p>
            <p className="mt-4 inline-block rounded-lg border border-white/8 bg-white/[0.03] px-3 py-1 font-mono text-xs text-fog/50">
              {s.detail}
            </p>
          </Reveal>
        ))}
      </div>
    </section>
  );
}
