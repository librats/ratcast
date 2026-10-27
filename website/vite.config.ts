import { fileURLToPath } from "node:url";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
import { defineConfig } from "vite";

const repo = fileURLToPath(new URL("..", import.meta.url));

// The logo and the app's icon are the repository's own (docs/, app/resources/), imported
// through @repo so the site never keeps copies of them.
export default defineConfig({
  base: "/",
  plugins: [react(), tailwindcss()],
  resolve: { alias: { "@repo": repo } },
  server: { fs: { allow: [repo] } },
});
