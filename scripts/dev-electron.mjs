import { spawn } from "node:child_process";
import http from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(__dirname, "..");
const url = "http://127.0.0.1:5173";
const viteBin = path.join(root, "node_modules", "vite", "bin", "vite.js");
const electronBin = process.platform === "win32"
  ? path.join(root, "node_modules", ".bin", "electron.cmd")
  : path.join(root, "node_modules", ".bin", "electron");

function spawnChild(command, args, env = {}) {
  return spawn(command, args, {
    cwd: root,
    env: { ...process.env, ...env },
    stdio: "inherit",
    shell: false
  });
}

function waitForServer(targetUrl, timeoutMs = 30000) {
  const deadline = Date.now() + timeoutMs;

  return new Promise((resolve, reject) => {
    const tick = () => {
      const request = http.get(targetUrl, (response) => {
        response.resume();
        resolve();
      });

      request.on("error", () => {
        if (Date.now() > deadline) {
          reject(new Error(`Timed out waiting for ${targetUrl}`));
          return;
        }
        setTimeout(tick, 250);
      });
      request.setTimeout(1000, () => request.destroy());
    };

    tick();
  });
}

const vite = spawnChild(process.execPath, [viteBin, "--host", "127.0.0.1"]);

try {
  await waitForServer(url);
} catch (error) {
  console.error(error.message);
  vite.kill();
  process.exit(1);
}

const electron = spawnChild(electronBin, ["."], {
  WAPI_IDE_DEV_SERVER_URL: url
});

electron.on("exit", (code) => {
  vite.kill();
  process.exit(code ?? 0);
});

process.on("SIGINT", () => {
  electron.kill();
  vite.kill();
});
