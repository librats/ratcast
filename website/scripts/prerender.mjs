// Puts each page's HTML into its file in dist/ (index.html, privacy/index.html, and the pages of
// links, r/, u/ and d/ index.html), so search engines and link previews read its text without
// running scripts; the browser then hydrates it (src/main.tsx, src/privacy.tsx, src/opener.tsx).
import { readFile, rm, writeFile } from "node:fs/promises";

const server = new URL("../dist-server/", import.meta.url);
const { pages, render } = await import(new URL("entry-server.js", server).href);

// The root, empty, its attributes kept (a page of links says its kind on it).
const marker = /<div id="root"([^>]*)><\/div>/;
for (const page of pages) {
  const file = new URL(`../dist/${page}`, import.meta.url);
  const html = await readFile(file, "utf8");
  if (!marker.test(html)) throw new Error(`prerender: no empty root in dist/${page}`);
  await writeFile(file, html.replace(marker, (_, attributes) => `<div id="root"${attributes}>${render(page)}</div>`));
  console.log(`prerendered dist/${page}`);
}
await rm(server, { recursive: true, force: true });
