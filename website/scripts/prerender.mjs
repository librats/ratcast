// Puts the page's HTML into dist/index.html, so search engines and link previews read its
// text without running scripts; the browser then hydrates it (src/main.tsx).
import { readFile, rm, writeFile } from "node:fs/promises";

const server = new URL("../dist-server/", import.meta.url);
const index = new URL("../dist/index.html", import.meta.url);
const { render } = await import(new URL("entry-server.js", server).href);

const marker = '<div id="root"></div>';
const html = await readFile(index, "utf8");
if (!html.includes(marker)) throw new Error(`prerender: no ${marker} in dist/index.html`);
await writeFile(index, html.replace(marker, `<div id="root">${render()}</div>`));
await rm(server, { recursive: true, force: true });
console.log("prerendered dist/index.html");
