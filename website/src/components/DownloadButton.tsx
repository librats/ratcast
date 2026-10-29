import { Apple, ArrowDownToLine } from "lucide-react";
import { formatSize, RELEASES_URL, useLatestRelease, usePlatform } from "../lib/release";
import { LinuxIcon, WindowsIcon } from "./ui";

const NAMES = { windows: "Windows", linux: "Linux", mac: "macOS" } as const;
const FORMATS = { windows: "zip", linux: "AppImage", mac: "DMG" } as const;

/** The big button: the package for the visitor's system, straight from the latest release. */
export function DownloadButton() {
  const release = useLatestRelease();
  const platform = usePlatform();
  const target = platform === "windows" || platform === "linux" || platform === "mac" ? platform : null;
  const pkg = target ? release?.packages.find((p) => p.platform === target) : undefined;

  const href = pkg ? pkg.url : target ? RELEASES_URL : "#download";
  const Icon =
    target === "linux" ? LinuxIcon : target === "windows" ? WindowsIcon : target === "mac" ? Apple : ArrowDownToLine;
  const label = target ? `Download for ${NAMES[target]}` : "Download ratcast";
  const detail = pkg
    ? `v${release!.version} · ${FORMATS[target!]} · ${formatSize(pkg.size)}`
    : target
      ? "Latest release on GitHub"
      : "Windows, Linux & macOS · Android planned";

  return (
    <a
      href={href}
      className="group relative inline-flex items-center gap-4 overflow-hidden rounded-2xl bg-fog py-3 pr-6 pl-4 text-ink shadow-[0_0_60px_-12px] shadow-cyan/50 transition hover:shadow-[0_0_80px_-8px] hover:shadow-violet/60"
    >
      {/* a sheen that passes over on hover */}
      <span
        aria-hidden
        className="absolute inset-y-0 -left-1/2 w-1/3 -skew-x-12 bg-gradient-to-r from-transparent via-white/80 to-transparent opacity-0 transition-all duration-700 group-hover:left-[120%] group-hover:opacity-100"
      />
      <span className="grid size-10 place-items-center rounded-xl bg-ink text-fog">
        <Icon className="size-5" />
      </span>
      <span className="relative flex flex-col text-left">
        <span className="font-semibold">{label}</span>
        <span className="font-mono text-xs text-ink/60">{detail}</span>
      </span>
    </a>
  );
}
