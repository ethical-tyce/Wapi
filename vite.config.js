import { defineConfig } from "vite";

export default defineConfig({
  base: "./",
  clearScreen: false,
  server: {
    host: "127.0.0.1",
    port: 5173,
    strictPort: true,
    watch: {
      ignored: ["**/src-tauri/**"]
    }
  },
  build: {
    target: "es2022",
    outDir: "dist",
    emptyOutDir: true,
    chunkSizeWarningLimit: 3000,
    rollupOptions: {
      output: {
        manualChunks: {
          monaco: ["monaco-editor"],
          terminal: ["@xterm/xterm", "@xterm/addon-fit"],
          icons: ["lucide"]
        }
      }
    }
  }
});
