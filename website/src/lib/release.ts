import { useEffect, useState } from "react";

export const REPO = "librats/ratcast";
export const REPO_URL = `https://github.com/${REPO}`;
export const RELEASES_URL = `${REPO_URL}/releases/latest`;

export type Platform = "windows" | "linux" | "mac" | "android" | "other";

export interface Package {
  platform: "windows" | "linux";
  name: string;
  url: string;
  size: number;
}

export interface Release {
  version: string;
  url: string;
  published: string;
  packages: Package[];
}

// Package names as release.yml makes them (and app/update/release.h finds them):
// ratcast-<version>-windows-x64.zip, ratcast-<version>-linux-x86_64.AppImage.
const PACKAGES: { platform: Package["platform"]; pattern: RegExp }[] = [
  { platform: "windows", pattern: /^ratcast-.+-windows-x64\.zip$/ },
  { platform: "linux", pattern: /^ratcast-.+-linux-x86_64\.AppImage$/ },
];

interface GitHubAsset {
  name: string;
  browser_download_url: string;
  size: number;
}

interface GitHubRelease {
  tag_name: string;
  html_url: string;
  published_at: string;
  assets: GitHubAsset[];
}

function parse(release: GitHubRelease): Release {
  const packages: Package[] = [];
  for (const { platform, pattern } of PACKAGES) {
    const asset = release.assets.find((a) => pattern.test(a.name));
    if (asset) packages.push({ platform, name: asset.name, url: asset.browser_download_url, size: asset.size });
  }
  return {
    version: release.tag_name.replace(/^v/, ""),
    url: release.html_url,
    published: release.published_at,
    packages,
  };
}

const asked = new Map<string, Promise<unknown>>();

// GitHub lets a browser ask 60 times an hour, so one answer is kept for the session and
// every component that wants it shares the one request.
function cached<T>(key: string, load: () => Promise<T>): Promise<T> {
  let promise = asked.get(key) as Promise<T> | undefined;
  if (!promise) {
    promise = (async () => {
      try {
        const kept = sessionStorage.getItem(key);
        if (kept) return JSON.parse(kept) as T;
      } catch {
        /* storage may be blocked; ask again */
      }
      const value = await load();
      try {
        sessionStorage.setItem(key, JSON.stringify(value));
      } catch {
        /* nothing to keep it in */
      }
      return value;
    })();
    asked.set(key, promise);
  }
  return promise;
}

async function github<T>(path: string): Promise<T> {
  const response = await fetch(`https://api.github.com/repos/${REPO}${path}`, {
    headers: { Accept: "application/vnd.github+json" },
  });
  if (!response.ok) throw new Error(`GitHub: ${response.status}`);
  return (await response.json()) as T;
}

export function useLatestRelease(): Release | null {
  const [release, setRelease] = useState<Release | null>(null);
  useEffect(() => {
    cached("ratcast.release", async () => parse(await github<GitHubRelease>("/releases/latest")))
      .then(setRelease)
      .catch(() => setRelease(null));
  }, []);
  return release;
}

export function useStars(): number | null {
  const [stars, setStars] = useState<number | null>(null);
  useEffect(() => {
    cached("ratcast.stars", async () => (await github<{ stargazers_count: number }>("")).stargazers_count)
      .then(setStars)
      .catch(() => setStars(null));
  }, []);
  return stars;
}

export function detectPlatform(): Platform {
  const nav = navigator as Navigator & { userAgentData?: { platform?: string } };
  const name = (nav.userAgentData?.platform || navigator.userAgent).toLowerCase();
  if (name.includes("android")) return "android";
  if (name.includes("win")) return "windows";
  if (name.includes("mac") || name.includes("iphone") || name.includes("ipad")) return "mac";
  if (name.includes("linux") || name.includes("x11")) return "linux";
  return "other";
}

export function formatSize(bytes: number): string {
  return `${(bytes / 1024 / 1024).toFixed(0)} MB`;
}
