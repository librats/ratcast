import { motion, useMotionTemplate, useMotionValue } from "motion/react";
import type { ReactNode, MouseEvent } from "react";

export function Reveal({ children, delay = 0, className }: { children: ReactNode; delay?: number; className?: string }) {
  return (
    <motion.div
      className={className}
      initial={{ opacity: 0, y: 24, filter: "blur(6px)" }}
      whileInView={{ opacity: 1, y: 0, filter: "blur(0px)" }}
      viewport={{ once: true, margin: "-80px" }}
      transition={{ duration: 0.7, delay, ease: [0.22, 1, 0.36, 1] }}
    >
      {children}
    </motion.div>
  );
}

export function SectionHeading({ eyebrow, title, children }: { eyebrow: string; title: ReactNode; children?: ReactNode }) {
  return (
    <Reveal className="mx-auto max-w-2xl text-center">
      <p className="font-mono text-xs uppercase tracking-[0.25em] text-cyan">{eyebrow}</p>
      <h2 className="mt-4 text-4xl font-semibold tracking-tight text-balance md:text-5xl">{title}</h2>
      {children && <p className="mt-5 text-lg text-pretty text-fog/60">{children}</p>}
    </Reveal>
  );
}

/** A card with a light that follows the pointer across it and its border. */
export function SpotlightCard({ children, className = "" }: { children: ReactNode; className?: string }) {
  const x = useMotionValue(-400);
  const y = useMotionValue(-400);
  const light = useMotionTemplate`radial-gradient(420px circle at ${x}px ${y}px, rgb(139 92 246 / 0.16), transparent 70%)`;
  const edge = useMotionTemplate`radial-gradient(260px circle at ${x}px ${y}px, rgb(34 211 238 / 0.6), transparent 70%)`;

  function move(event: MouseEvent<HTMLDivElement>) {
    const box = event.currentTarget.getBoundingClientRect();
    x.set(event.clientX - box.left);
    y.set(event.clientY - box.top);
  }

  return (
    <div
      onMouseMove={move}
      onMouseLeave={() => {
        x.set(-400);
        y.set(-400);
      }}
      className={`group relative overflow-hidden rounded-3xl border border-white/8 bg-white/[0.025] p-px ${className}`}
    >
      <motion.div
        aria-hidden
        className="pointer-events-none absolute inset-0 rounded-3xl"
        style={{
          background: edge,
          mask: "linear-gradient(#000 0 0) content-box exclude, linear-gradient(#000 0 0)",
          padding: 1,
        }}
      />
      <motion.div aria-hidden className="pointer-events-none absolute inset-0" style={{ background: light }} />
      <div className="relative h-full rounded-[calc(1.5rem-1px)] p-6 md:p-7">{children}</div>
    </div>
  );
}

export function GitHubIcon({ className = "size-5" }: { className?: string }) {
  return (
    <svg viewBox="0 0 24 24" fill="currentColor" aria-hidden className={className}>
      <path d="M12 .5C5.65.5.5 5.65.5 12c0 5.08 3.29 9.39 7.86 10.91.58.1.79-.25.79-.56v-2c-3.2.7-3.87-1.37-3.87-1.37-.52-1.33-1.28-1.69-1.28-1.69-1.04-.71.08-.7.08-.7 1.15.08 1.76 1.18 1.76 1.18 1.03 1.76 2.69 1.25 3.35.96.1-.75.4-1.25.73-1.54-2.55-.29-5.24-1.28-5.24-5.69 0-1.26.45-2.29 1.18-3.09-.12-.29-.51-1.46.11-3.05 0 0 .97-.31 3.17 1.18a11 11 0 0 1 5.77 0c2.2-1.49 3.17-1.18 3.17-1.18.62 1.59.23 2.76.11 3.05.74.8 1.18 1.83 1.18 3.09 0 4.42-2.69 5.39-5.25 5.68.41.36.78 1.06.78 2.14v3.17c0 .31.21.67.8.56A11.5 11.5 0 0 0 23.5 12C23.5 5.65 18.35.5 12 .5Z" />
    </svg>
  );
}

export function WindowsIcon({ className = "size-5" }: { className?: string }) {
  return (
    <svg viewBox="0 0 24 24" fill="currentColor" aria-hidden className={className}>
      <path d="M2 4.6 9.8 3.5v7.7H2V4.6Zm0 14.8 7.8 1.1v-7.6H2v6.5Zm8.7 1.2L22 22V12.9H10.7v7.7Zm0-17.2v7.8H22V2L10.7 3.4Z" />
    </svg>
  );
}

export function LinuxIcon({ className = "size-5" }: { className?: string }) {
  return (
    <svg viewBox="0 0 24 24" fill="currentColor" aria-hidden className={className}>
      <path d="M12.5 2c-2.2 0-3.4 1.8-3.4 4.1 0 1.4.2 2.3-.6 3.6-1 1.6-2.6 3.5-2.9 6-.1.9.1 1.6.4 2.1-.6.4-1.5.6-1.7 1.3-.3 1 .9 1.4 2.3 1.8 1.2.3 2.2 1.1 3 1.1.9 0 1.4-.6 1.9-.9h1.9c.5.3 1 .9 1.9.9.8 0 1.8-.8 3-1.1 1.4-.4 2.6-.8 2.3-1.8-.2-.7-1.1-.9-1.7-1.3.3-.5.5-1.2.4-2.1-.3-2.5-1.9-4.4-2.9-6-.8-1.3-.6-2.2-.6-3.6C15.8 3.8 14.7 2 12.5 2Zm-1.4 3.5c.5 0 .8.6.8 1.2 0 .3-.1.5-.2.7l-.6-.2c.1-.1.1-.3.1-.5 0-.3-.1-.5-.3-.5s-.3.2-.3.5v.2l-.5-.2v-.1c0-.6.4-1.1.9-1.1Zm2.6 0c.6 0 1 .5 1 1.2v.3l-.6.2v-.4c0-.3-.2-.6-.4-.6-.3 0-.4.3-.4.6v.2l-.5-.2c0-.2-.1-.3-.1-.4 0-.5.4-.9 1-.9Zm-1.3 2.2c.8 0 1.9.4 2.1.7.1.1.1.3-.1.5-.5.4-1.2.9-2 .9s-1.4-.4-2-.9c-.2-.1-.2-.3-.1-.4.4-.4 1.3-.8 2.1-.8Z" />
    </svg>
  );
}
