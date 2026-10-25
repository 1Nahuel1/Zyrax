import { defineConfig } from "vite";

// https://vitejs.dev/config/
export default defineConfig(async () => ({
  clearScreen: false,
  server: {
    port: 5173,
    strictPort: true,
    watch: {
      ignored: ["**/src-tauri/**"],
    },
  },
  envPrefix: ["VITE_", "TAURI_"],
}));
