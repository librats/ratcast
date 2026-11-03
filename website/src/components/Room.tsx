import { ArrowLeft, Check, Copy, ExternalLink } from "lucide-react";
import { useEffect, useState } from "react";
import { DownloadButton } from "./DownloadButton";
import { Logo } from "./Nav";

/**
 * The room's link carried after '#': the browser never sends it to a server. A web link is
 * "https://ratcast.app/r/#" and the rats:// link without its scheme (to_web_link() in
 * client/include/ratcast/client/rats_url.h: keep them in step), escaped or not.
 */
function roomLink(hash: string): string | null {
  let carried = hash.replace(/^#/, "");
  try {
    carried = decodeURIComponent(carried);
  } catch {
    // as it came
  }
  carried = carried.trim();
  return carried ? `rats://${carried}` : null;
}

/**
 * A link to a room (/r/#…), as it opens where ratcast is not reached by itself: the app is opened
 * through rats:// at once, with a button to do it again and the download for one without it. On
 * Android, App Links open the app before this page. Prerendered without the link, which only the
 * browser knows: it is read in an effect.
 */
export function Room() {
  // undefined: not read yet (the prerendered page); null: none.
  const [link, setLink] = useState<string | null | undefined>(undefined);
  const [copied, setCopied] = useState(false);

  useEffect(() => {
    const found = roomLink(window.location.hash);
    setLink(found);
    if (found) window.location.href = found;
  }, []);

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
        <p className="font-mono text-xs uppercase tracking-[0.25em] text-cyan">Invitation</p>
        <h1 className="mt-4 text-4xl font-semibold tracking-tight text-balance md:text-5xl">
          {missing ? "This link is not whole." : "You're invited to a room."}
        </h1>
        <p className="mt-5 text-lg text-pretty text-fog/60">
          {missing
            ? "Everything after the # is the room. Ask for the link again, and send it as it is."
            : "ratcast should be opening. If it does not, open it here."}
        </p>

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
          <p className="mt-2 text-fog/60">
            It is free, with no account. Install it, then open the link again.
          </p>
          <div className="mt-6">
            <DownloadButton />
          </div>
          {link && (
            <>
              <p className="mt-8 text-sm text-fog/50">Or paste the room's link into the app:</p>
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
          The room's link stays in your browser: this page never sends it anywhere.{" "}
          <a href="/privacy/" className="underline-offset-4 hover:text-fog/70 hover:underline">
            Privacy
          </a>
        </p>
      </main>
    </>
  );
}
