import { MotionConfig } from "motion/react";
import { StrictMode } from "react";
import { renderToString } from "react-dom/server";
import App from "./App";
import { Privacy } from "./components/Privacy";
import { Room } from "./components/Room";

/** The pages, by the HTML file each is put into: dist/index.html, dist/privacy/index.html, … */
const PAGES = {
  "index.html": () => <App />,
  "privacy/index.html": () => (
    <MotionConfig reducedMotion="user">
      <Privacy />
    </MotionConfig>
  ),
  "r/index.html": () => <Room />,
} as const;

export const pages = Object.keys(PAGES) as (keyof typeof PAGES)[];

/** A page as HTML, for scripts/prerender.mjs to put into its file. */
export function render(page: keyof typeof PAGES): string {
  const Page = PAGES[page];
  return renderToString(
    <StrictMode>
      <Page />
    </StrictMode>,
  );
}
