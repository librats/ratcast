import { MotionConfig } from "motion/react";
import { StrictMode } from "react";
import { createRoot, hydrateRoot } from "react-dom/client";
import { Privacy } from "./components/Privacy";
import "./index.css";

const root = document.getElementById("root")!;
const page = (
  <StrictMode>
    <MotionConfig reducedMotion="user">
      <Privacy />
    </MotionConfig>
  </StrictMode>
);

// Prerendered like the main page (scripts/prerender.mjs), except on the dev server.
if (root.hasChildNodes()) hydrateRoot(root, page);
else createRoot(root).render(page);
