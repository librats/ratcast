import { StrictMode } from "react";
import { createRoot, hydrateRoot } from "react-dom/client";
import { type LinkKind, Opener } from "./components/Opener";
import "./index.css";

// The pages of links (r/, u/, d/ index.html), each saying its kind on its root.
const root = document.getElementById("root")!;
const kind = (root.dataset.kind ?? "room") as LinkKind;
const page = (
  <StrictMode>
    <Opener kind={kind} />
  </StrictMode>
);

// Prerendered like the main page (scripts/prerender.mjs), except on the dev server.
if (root.hasChildNodes()) hydrateRoot(root, page);
else createRoot(root).render(page);
