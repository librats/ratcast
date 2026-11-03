import { ArrowLeft, Check, Copy, ExternalLink } from "lucide-react";
import { useEffect, useState } from "react";
import { DownloadButton } from "./DownloadButton";
import { Logo } from "./Nav";

/** The links the site opens in the app: a room's (/r/), a person's (/u/), a device's (/d/). */
export type LinkKind = "room" | "friend" | "device";

/**
 * Each kind's rats:// link: what follows '#' after this. Keep them in step with the app's:
 * to_web_link() in client/include/ratcast/client/rats_url.h (a room's) and people.h (a person's,
 * a device's).
 */
const SCHEMES: Record<LinkKind, string> = {
  room: "rats://",
  friend: "rats://u/",
  device: "rats://d/",
};

const TEXT: Record<LinkKind, { eyebrow: string; title: string; body: string; missing: string }> = {
  room: {
    eyebrow: "Invitation",
    title: "You're invited to a room.",
    body: "ratcast should be opening. If it does not, open it here.",
    missing: "Everything after the # is the room. Ask for the link again, and send it as it is.",
  },
  friend: {
    eyebrow: "Friend request",
    title: "Someone wants to be your friend.",
    body: "ratcast should be opening, to ask them. If it does not, open it here.",
    missing: "Everything after the # is the person. Ask for the link again, and send it as it is.",
  },
  device: {
    eyebrow: "Another device",
    title: "Make this device yours.",
    body: "Open this on the device that is to be you: ratcast there asks the one that made the link. If it does not open, open it here.",
    missing: "Everything after the # is the device link. Make it again, and open it as it is.",
  },
};

/**
 * What a link carries after '#', which the browser never sends to a server, as its rats:// link;
 * escaped or not.
 */
function linkOf(kind: LinkKind, hash: string): string | null {
  let carried = hash.replace(/^#/, "");
  try {
    carried = decodeURIComponent(carried);
  } catch {
    // as it came
  }
  carried = carried.trim();
  return carried ? SCHEMES[kind] + carried : null;
}

/**
 * A link as it opens where ratcast is not reached by itself: the app is opened through rats:// at
 * once, with a button to do it again and the download for one without it. On Android, App Links
 * open the app before this page. Prerendered without the link, which only the browser knows: it is
 * read in an effect.
 */
export function Opener({ kind }: { kind: LinkKind }) {
  // undefined: not read yet (the prerendered page); null: none.
  const [link, setLink] = useState<string | null | undefined>(undefined);
  const [copied, setCopied] = useState(false);
  const text = TEXT[kind];

  useEffect(() => {
    const found = linkOf(kind, window.location.hash);
    setLink(found);
    if (found) window.location.href = found;
  }, [kind]);

  async function copy() {
    if (!link) return;
    try {
      await navigator.clipboard.writeText(link);
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    } catch {
      // the clipboard refused: the link is there to select
    }
  }

  const missing = link === null;

  return (
    <>
      <header className="flex justify-center px-4 pt-4">
        <nav className="flex w-full max-w-6xl items-center justify-between gap-4 px-4 py-2.5">
          <Logo href="/" />
          <a
            href="/"
            className="flex items-center gap-2 rounded-xl border border-white/10 px-3 py-1.5 text-sm text-fog/80 transition hover:border-white/20 hover:text-fog"
          >
            <ArrowLeft className="size-4" />
            About ratcast
          </a>
        </nav>
      </header>

      <main className="mx-auto flex max-w-2xl flex-col items-center px-4 pt-20 pb-24 text-center md:pt-28">
        <p className="font-mono text-xs uppercase tracking-[0.25em] text-cyan">{text.eyebrow}</p>
        <h1 className="mt-4 text-4xl font-semibold tracking-tight text-balance md:text-5xl">
          {missing ? "This link is not whole." : text.title}
        </h1>
        <p className="mt-5 text-lg text-pretty text-fog/60">{missing ? text.missing : text.body}</p>

        {!missing && (
          <a
            href={link ?? undefined}
            aria-disabled={link === undefined}
            className="mt-8 inline-flex items-center gap-2 rounded-2xl bg-fog px-6 py-3 font-semibold text-ink shadow-[0_0_60px_-12px] shadow-cyan/50 transition hover:shadow-[0_0_80px_-8px] hover:shadow-violet/60 aria-disabled:pointer-events-none aria-disabled:opacity-50"
          >
            <ExternalLink className="size-5" />
            Open in ratcast
          </a>
        )}

        <div className="mt-16 w-full rounded-3xl border border-white/8 bg-white/[0.025] p-6 md:p-8">
          <h2 className="text-xl font-semibold tracking-tight">No ratcast yet?</h2>
          <p className="mt-2 text-fog/60">It is free, with no account. Install it, then open the link again.</p>
          <div className="mt-6">
            <DownloadButton />
          </div>
          {link && (
            <>
              <p className="mt-8 text-sm text-fog/50">Or paste the link into the app:</p>
              <div className="mt-2 flex items-center gap-2 rounded-xl border border-white/10 bg-ink-2 py-1.5 pr-1.5 pl-3">
                <code className="min-w-0 flex-1 truncate text-left font-mono text-xs text-fog/80 select-all">
                  {link}
                </code>
                <button
                  type="button"
                  onClick={copy}
                  className="flex items-center gap-1.5 rounded-lg px-2.5 py-1.5 text-sm text-fog/70 transition hover:bg-white/5 hover:text-fog"
                >
                  {copied ? <Check className="size-4 text-mint" /> : <Copy className="size-4" />}
                  {copied ? "Copied" : "Copy"}
                </button>
              </div>
            </>
          )}
        </div>

        <p className="mt-12 text-sm text-fog/40">
          The link stays in your browser: this page never sends it anywhere.{" "}
          <a href="/privacy/" className="underline-offset-4 hover:text-fog/70 hover:underline">
            Privacy
          </a>
        </p>
      </main>
    </>
  );
}
