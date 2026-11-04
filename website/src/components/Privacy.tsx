import { ArrowLeft } from "lucide-react";
import type { ReactNode } from "react";
import { REPO_URL } from "../lib/release";
import { Footer } from "./Footer";
import { Logo } from "./Nav";

/** When the policy last changed: change it with the text. */
export const PRIVACY_UPDATED = "November 4, 2026";

function Section({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="mt-12">
      <h2 className="text-2xl font-semibold tracking-tight">{title}</h2>
      <div className="mt-4 space-y-4 text-fog/70 [&_a]:text-cyan [&_a]:underline-offset-4 [&_a:hover]:underline [&_li]:pl-1 [&_strong]:font-semibold [&_strong]:text-fog [&_ul]:list-disc [&_ul]:space-y-2 [&_ul]:pl-5">
        {children}
      </div>
    </section>
  );
}

/**
 * The privacy policy (/privacy), which Google Play asks an app that uses the microphone, the
 * camera and the screen to link to. Its facts are the app's: the permissions in
 * app/android/AndroidManifest.xml, the network librats reaches (the DHT's seeds, STUN, the
 * servers of RATCAST_SERVERS), what the data directory and the settings keep. Keep them true.
 */
export function Privacy() {
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
            Back to ratcast
          </a>
        </nav>
      </header>

      <main className="mx-auto max-w-3xl px-4 pt-16 pb-24 text-base leading-relaxed md:pt-24">
        <p className="font-mono text-xs uppercase tracking-[0.25em] text-cyan">Privacy policy</p>
        <h1 className="mt-4 text-4xl font-semibold tracking-tight text-balance md:text-5xl">
          Your call stays between you and the people in it.
        </h1>
        <p className="mt-5 text-sm text-fog/45">Last updated: {PRIVACY_UPDATED}</p>

        <p className="mt-8 text-lg text-pretty text-fog/70">
          This policy covers the ratcast app on Android, Windows, Linux and macOS, and this website. ratcast is free,
          open-source software made by the ratcast contributors. It has no accounts, no ads, no analytics and no
          tracking, and its makers do not collect, sell or share your personal data. Its{" "}
          <a href={REPO_URL} className="text-cyan underline-offset-4 hover:underline">
            source code
          </a>{" "}
          is public, so anyone can check what it does.
        </p>

        <Section title="What the app uses on your device">
          <p>ratcast asks only for what a call needs, and uses each thing only while you are in a call or a share:</p>
          <ul>
            <li>
              <strong>Microphone</strong>, so the others in the room hear you. You can mute it at any time. The sound is
              cleaned up (echo cancellation, noise suppression) on your device.
            </li>
            <li>
              <strong>Camera</strong>, only when you turn your camera on in a call.
            </li>
            <li>
              <strong>Screen and the sound of other apps</strong>, only when you start a share and confirm it in the
              system's own dialog. Android shows that the screen is being shared until you stop.
            </li>
            <li>
              <strong>Network access</strong>, to connect to the others in the room, and to your friends and your
              other devices.
            </li>
            <li>
              <strong>A foreground service and its notification</strong>, so a call or a share goes on while the app is
              in the background; <strong>audio settings</strong>, to route the call's sound like a phone call;{" "}
              <strong>keeping the screen on</strong> while you watch.
            </li>
          </ul>
          <p>
            The app does not read your contacts, files, photos, location or anything else on the device, and does not
            record calls.
          </p>
        </Section>

        <Section title="What is sent, and to whom">
          <p>
            What you say, show and share — your voice, your camera, your screen, your chat messages and the name others
            see you by (on a computer, your user name there, unless you change it) — goes only to the other members of that room. It is <strong>encrypted end to end</strong>:
            connections use the Noise protocol, and every audio and video frame is also sealed with SFrame, with keys
            only the members have. Nobody else, including the makers of ratcast and any server along the way, can read
            it.
          </p>
          <p>
            Chat messages are kept in the memory of the members' apps while the room is open, so that whoever joins
            later sees the last hundred. They are not saved to disk.
          </p>
          <p>
            Who joins a room is up to you: anyone who has a room's link can join it, so share it only with people
            you want there.
          </p>
        </Section>

        <Section title="Friends and your devices">
          <p>
            ratcast has no accounts. Who you are is a <strong>key made on your device</strong>, which nobody else has
            unless you give it: to another device of yours, by a link you make that you approve, or in a backup file
            you save. Your name, your friends and the requests between you are kept on your devices and passed only
            between them, encrypted, when they meet.
          </p>
          <ul>
            <li>
              The members of a room see your name, the public part of your key (as four characters beside your name)
              and a code that lets them ask to be your friend. Nobody becomes your friend unless you accept.
            </li>
            <li>
              Anyone you send your personal link to can ask to be your friend. You can make a new link at any time;
              the old one then stops working.
            </li>
            <li>
              While the app is running, it stays reachable for friend requests and your other devices: it connects to
              a ratcast server and is listed there under codes derived from your links, and it announces itself under
              the same codes in the public DHT, so that it can be found when no server is reached. The server and the
              DHT's devices see your IP address and those codes, not who you are or who your friends are: a code can
              only be worked out from a link. Your friends see whether you are online or in a room: your devices are
              listed at the server under a code only your friends can work out, and watch your friends' codes there,
              which tells the server how many devices are listed under each, never who they are.
            </li>
          </ul>
        </Section>

        <Section title="What the network sees">
          <p>
            ratcast connects devices directly to each other. Like any internet connection, that means some parties see
            your device's <strong>IP address</strong> and a random identifier the app makes for itself (its peer id),
            though never the content of a call:
          </p>
          <ul>
            <li>the other members of a room, whose apps connect to yours;</li>
            <li>
              the public distributed hash table (DHT), a network of other people's devices, through which rooms are
              found by their peer id, and your devices by those who have your links; the app joins it through well-known entry points such as{" "}
              <code className="font-mono text-sm">router.bittorrent.com</code>;
            </li>
            <li>
              public STUN servers (such as Google's <code className="font-mono text-sm">stun.l.google.com</code>), which
              tell the app the address the internet sees it at, and your router, which the app may ask to open a port;
            </li>
            <li>
              a ratcast server (by default <code className="font-mono text-sm">degitx.com</code>, run by the project, or
              one named in your settings or in a room's link), which helps devices behind strict networks meet and, when
              no direct path works, carries their encrypted traffic. It cannot decrypt what it carries.
            </li>
          </ul>
          <p>These addresses are used only to make and keep the connection.</p>
        </Section>

        <Section title="What is kept on your device">
          <ul>
            <li>the app's identity keys, from which its peer ids come, and your personal key;</li>
            <li>your name, your friends and the friend requests between you;</li>
            <li>the ratcast servers it has heard of;</li>
            <li>
              your settings: the language, the microphone, speaker and camera you chose, the volume you set for each
              member (by their key) and, on a computer, the screen or window you last shared.
            </li>
          </ul>
          <p>
            These stay on your devices and are never sent to the makers of ratcast. On Android they are left out of
            the system's backups. Uninstalling the app, or clearing its data, deletes them all; a backup file you
            saved yourself stays where you put it.
          </p>
        </Section>

        <Section title="Updates">
          <p>
            The Android app is updated through the store or the package you installed; it does not check for updates
            itself. The Windows, Linux and macOS packages ask GitHub for the latest release every few hours and download
            it from there, which GitHub sees as a request from your IP address (
            <a href="https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement">
              GitHub's privacy statement
            </a>
            ). You can turn this off in the app's settings.
          </p>
        </Section>

        <Section title="This website">
          <p>
            ratcast.app is a static site hosted on GitHub Pages. It sets no cookies and has no analytics. To show the
            latest release and the number of stars, your browser asks GitHub's public API, and GitHub, as the host,
            sees the requests your browser makes.
          </p>
          <p>
            A link to a room (ratcast.app/r/#…) carries the room after the #, which your browser never sends to any
            server: the page reads it on your device only to open the app.
          </p>
        </Section>

        <Section title="Children">
          <p>
            ratcast is not directed at children under 13, and its makers do not knowingly collect anything from them —
            or from anyone.
          </p>
        </Section>

        <Section title="Your choices and deleting data">
          <p>
            There is no account and nothing about you is held by the makers of ratcast, so there is nothing to request
            or delete on our side. You can refuse or withdraw the microphone and camera permissions in the system's
            settings at any time (the app then works without them), and remove everything the app keeps by uninstalling
            it or clearing its data.
          </p>
        </Section>

        <Section title="Changes">
          <p>
            If this policy changes, the new version is published on this page with a new date, and its history is
            public in the{" "}
            <a href={`${REPO_URL}/commits/main/website/src/components/Privacy.tsx`}>project's repository</a>.
          </p>
        </Section>

        <Section title="Contact">
          <p>
            Questions about privacy in ratcast: open an issue at{" "}
            <a href={`${REPO_URL}/issues`}>github.com/librats/ratcast/issues</a>.
          </p>
        </Section>
      </main>

      <Footer />
    </>
  );
}
