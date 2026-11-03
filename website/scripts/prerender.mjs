// Puts each page's HTML into its file in dist/ (index.html, privacy/index.html), so search
// engines and link previews read its text without running scripts; the browser then hydrates
// it (src/main.tsx, src/privacy.tsx).
import { readFile, rm, writeFile } from "node:fs/promises";

const server = new URL("../dist-server/", import.meta.url);
const { pages, render } = await import(new URL("entry-server.js", server).href);

const marker = '<div id="root"></div>';
for (const page of pages) {
  const file = new URL(`../dist/${page}`, import.meta.url);
  const html = await readFile(file, "utf8");
  if (!html.includes(marker)) throw new Error(`prerender: no ${marker} in dist/${page}`);
  await writeFile(file, html.replace(marker, `<div id="root">${render(page)}</div>`));
  console.log(`prerendered dist/${page}`);
}
await rm(server, { recursive: true, force: true });
