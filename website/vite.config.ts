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
  // The site, its privacy policy (/privacy/, which Google Play links to) and the pages links
  // open: a room's (/r/#…), a person's (/u/#…), a device's (/d/#…).
  build: {
    rolldownOptions: {
      input: {
        main: fileURLToPath(new URL("index.html", import.meta.url)),
        privacy: fileURLToPath(new URL("privacy/index.html", import.meta.url)),
        room: fileURLToPath(new URL("r/index.html", import.meta.url)),
        friend: fileURLToPath(new URL("u/index.html", import.meta.url)),
        device: fileURLToPath(new URL("d/index.html", import.meta.url)),
      },
    },
  },
  resolve: { alias: { "@repo": repo } },
  server: { fs: { allow: [repo] } },
});
