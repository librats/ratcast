import { Apple, ArrowDownToLine, Smartphone } from "lucide-react";
import type { ReactNode } from "react";
import { formatSize, RELEASES_URL, useLatestRelease, usePlatform, type Package } from "../lib/release";
import { CodeWindow } from "./Developers";
import { LinuxIcon, Reveal, SectionHeading, SpotlightCard, WindowsIcon } from "./ui";

const TARGETS: {
  platform: Package["platform"];
  name: string;
  icon: ReactNode;
  format: string;
  needs: string;
  steps: string;
}[] = [
  {
    platform: "windows",
    name: "Windows",
    icon: <WindowsIcon className="size-7" />,
    format: "x64 · zip",
    needs: "Windows 10 1903+ or 11. Sharing the applications' sound needs 2004+.",
    steps: "Unzip and run ratcast.exe",
  },
  {
    platform: "linux",
    name: "Linux",
    icon: <LinuxIcon className="size-7" />,
    format: "x86_64 · AppImage",
    needs: "glibc 2.39+ and PipeWire 1.0+: Ubuntu 24.04, Debian 13, Fedora 40 or newer.",
    steps: "chmod +x and run it",
  },
  {
    platform: "mac",
    name: "macOS",
    icon: <Apple className="size-7" />,
    format: "Apple Silicon · DMG · preview",
    needs: "macOS 14+ on Apple Silicon. Sharing the applications' sound needs 14.2+.",
    steps: "Drag ratcast into Applications. It is not notarized: open it the first time with right-click → Open",
  },
];

const SERVER = `git clone --recursive https://github.com/librats/ratcast.git && cd ratcast
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRATCAST_SERVER_ONLY=ON -DRATCAST_BUILD_TESTS=OFF
cmake --build build --parallel 2
sudo cmake --install build
# port 7700, UDP and TCP
sudo systemctl enable --now ratcast-server`;

export function Download() {
  const release = useLatestRelease();
  const mine = usePlatform();

  return (
    <section id="download" className="relative scroll-mt-24 overflow-hidden py-24">
      <div aria-hidden className="absolute inset-x-0 top-0 -z-10 h-[40rem] bg-[radial-gradient(ellipse_at_top,rgb(34_211_238/0.12),transparent_60%)]" />
      <div className="mx-auto max-w-6xl px-4">
        <SectionHeading eyebrow="Download" title="Get ratcast">
          {release
            ? `Version ${release.version}, released ${new Date(release.published).toLocaleDateString("en", { dateStyle: "long" })}.`
            : "The latest build, straight from GitHub Releases."}{" "}
          Free and open source. It keeps itself up to date.
        </SectionHeading>

        <div className="mt-14 grid gap-4 md:grid-cols-2 lg:grid-cols-3">
          {TARGETS.map((t, i) => {
            const pkg = release?.packages.find((p) => p.platform === t.platform);
            const yours = mine === t.platform;
            return (
              <Reveal key={t.platform} delay={i * 0.1}>
                <SpotlightCard className={`h-full ${yours ? "border-cyan/30" : ""}`}>
                  <div className="flex items-start justify-between">
                    <div className="grid size-14 place-items-center rounded-2xl bg-gradient-to-br from-white/15 to-white/0 ring-1 ring-white/10">
                      {t.icon}
                    </div>
                    {yours && (
                      <span className="rounded-full border border-cyan/30 bg-cyan/10 px-2.5 py-0.5 text-xs text-cyan">
                        Your system
                      </span>
                    )}
                  </div>
                  <h3 className="mt-6 text-2xl font-semibold tracking-tight">{t.name}</h3>
                  <p className="font-mono text-sm text-fog/50">
                    {t.format}
                    {pkg && ` · ${formatSize(pkg.size)}`}
                  </p>
                  <p className="mt-4 text-sm text-fog/60">{t.needs}</p>
                  <p className="mt-1 text-sm text-fog/40">{t.steps}.</p>
                  <a
                    href={pkg?.url ?? RELEASES_URL}
                    className={`mt-7 flex items-center justify-center gap-2 rounded-xl px-4 py-3 text-sm font-medium transition ${
                      yours
                        ? "bg-fog text-ink hover:bg-white"
                        : "border border-white/10 bg-white/5 hover:border-white/25 hover:bg-white/10"
                    }`}
                  >
                    <ArrowDownToLine className="size-4" />
                    {pkg ? pkg.name : `Download for ${t.name}`}
                  </a>
                </SpotlightCard>
              </Reveal>
            );
          })}
        </div>

        <Reveal delay={0.2} className="mt-4 flex flex-col items-center justify-between gap-3 rounded-2xl border border-dashed border-white/10 px-6 py-4 text-sm text-fog/50 sm:flex-row">
          <span className="flex items-center gap-3">
            <Smartphone className="size-4" />
            Android is planned.
          </span>
          <a href={release?.url ?? RELEASES_URL} className="text-cyan hover:underline">
            All files, checksums and notes →
          </a>
        </Reveal>

        <div className="mt-20 grid items-center gap-10 lg:grid-cols-[1fr_1.4fr]">
          <Reveal className="min-w-0">
            <p className="font-mono text-xs tracking-[0.25em] text-cyan uppercase">Optional</p>
            <h3 className="mt-4 text-3xl font-semibold tracking-tight">Run your own server</h3>
            <p className="mt-4 text-fog/60">
              Calls usually connect directly. A <code className="font-mono text-fog/80">ratcast-server</code> on a public
              host helps the rest: rooms behind strict NATs meet there, it relays when no path can be punched, and it can
              forward a share so the sharer uploads one copy however many watch. It can't read anything it carries.
            </p>
            <p className="mt-4 text-sm text-fog/40">Needs only librats and libratcast: no media libraries, no Qt.</p>
          </Reveal>
          <Reveal delay={0.1} className="min-w-0">
            <CodeWindow title="server" lang="sh" code={SERVER} />
          </Reveal>
        </div>
      </div>
    </section>
  );
}
