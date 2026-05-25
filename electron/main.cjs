const { app, BrowserWindow, dialog, ipcMain } = require("electron");
const fs = require("node:fs/promises");
const path = require("node:path");
const { spawn } = require("node:child_process");

const isDev = Boolean(process.env.WAPI_IDE_DEV_SERVER_URL);
const appIcon = path.join(app.getAppPath(), "icon.ico");
const projectFileExtensions = new Set([".wapi", ".txt", ".cpp", ".c", ".h", ".hpp"]);

function createWindow() {
  const win = new BrowserWindow({
    width: 1280,
    height: 820,
    minWidth: 980,
    minHeight: 640,
    backgroundColor: "#020403",
    frame: false,
    icon: appIcon,
    title: "Wapi IDE",
    show: false,
    webPreferences: {
      preload: path.join(__dirname, "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false
    }
  });

  win.once("ready-to-show", () => win.show());

  if (isDev) {
    win.loadURL(process.env.WAPI_IDE_DEV_SERVER_URL);
  } else {
    win.loadFile(path.join(__dirname, "..", "dist", "index.html"));
  }
}

function candidateExecutables() {
  const roots = [
    process.env.WAPI_EXE ? path.dirname(process.env.WAPI_EXE) : null,
    process.cwd(),
    app.getAppPath(),
    path.join(app.getAppPath(), "..")
  ].filter(Boolean);

  const candidates = process.env.WAPI_EXE ? [process.env.WAPI_EXE] : [];
  for (const root of roots) {
    candidates.push(
      path.join(root, "x64", "Debug", "Wapi.exe"),
      path.join(root, "x64", "Release", "Wapi.exe"),
      path.join(root, "ARM64", "Debug", "Wapi.exe"),
      path.join(root, "ARM64", "Release", "Wapi.exe"),
      path.join(root, "Wapi.exe")
    );
  }

  return [...new Set(candidates)];
}

async function fileExists(filePath) {
  try {
    await fs.access(filePath);
    return true;
  } catch {
    return false;
  }
}

async function resolveWapiExecutable() {
  for (const candidate of candidateExecutables()) {
    if (await fileExists(candidate)) return candidate;
  }
  return null;
}

async function readIdeFile(filePath, rootPath = null) {
  const source = await fs.readFile(filePath, "utf8");
  return {
    filePath,
    source,
    name: path.basename(filePath),
    relativePath: rootPath ? path.relative(rootPath, filePath) : path.basename(filePath)
  };
}

async function collectProjectFiles(rootPath, dirPath = rootPath, files = []) {
  const entries = await fs.readdir(dirPath, { withFileTypes: true });
  for (const entry of entries) {
    const nextPath = path.join(dirPath, entry.name);
    if (entry.isDirectory()) {
      if (![".git", "node_modules", "dist", "build", "x64", "ARM64"].includes(entry.name)) {
        await collectProjectFiles(rootPath, nextPath, files);
      }
    } else if (projectFileExtensions.has(path.extname(entry.name).toLowerCase())) {
      files.push(await readIdeFile(nextPath, rootPath));
    }
  }
  return files;
}

function buildArgs(command, source, options = {}) {
  const args = [command, source];
  if (options.mode) args.push("--mode", options.mode);
  if (options.allowInjection) args.push("--allow-injection");
  if (options.strictPermissions) args.push("--strict-permissions");
  for (const cap of options.capabilities || []) {
    if (cap) args.push("--cap", cap);
  }
  return args;
}

function runProcess(exe, args) {
  return new Promise((resolve) => {
    const child = spawn(exe, args, {
      cwd: path.dirname(exe),
      windowsHide: true
    });

    let stdout = "";
    let stderr = "";

    child.stdout.on("data", (chunk) => {
      stdout += chunk.toString();
    });
    child.stderr.on("data", (chunk) => {
      stderr += chunk.toString();
    });
    child.on("error", (error) => {
      resolve({
        ok: false,
        code: null,
        stdout,
        stderr: error.message,
        exe
      });
    });
    child.on("close", (code) => {
      resolve({
        ok: code === 0,
        code,
        stdout,
        stderr,
        exe
      });
    });
  });
}

ipcMain.handle("wapi:execute", async (_event, payload) => {
  const command = payload?.command === "run" ? "run" : "check";
  const source = typeof payload?.source === "string" ? payload.source : "";
  const options = payload?.options || {};
  const exe = await resolveWapiExecutable();

  if (!exe) {
    return {
      ok: false,
      code: null,
      stdout: "",
      stderr: "Wapi.exe was not found. Build the C++ project or set WAPI_EXE to the executable path.",
      exe: null
    };
  }

  return runProcess(exe, buildArgs(command, source, options));
});

ipcMain.handle("wapi:locate", async () => resolveWapiExecutable());

ipcMain.handle("wapi:addFiles", async () => {
  const result = await dialog.showOpenDialog({
    title: "Add files to solution",
    filters: [
      { name: "Wapi and source files", extensions: ["wapi", "txt", "cpp", "c", "h", "hpp"] },
      { name: "All files", extensions: ["*"] }
    ],
    properties: ["openFile", "multiSelections"]
  });

  if (result.canceled || result.filePaths.length === 0) return [];
  return Promise.all(result.filePaths.map((filePath) => readIdeFile(filePath)));
});

ipcMain.handle("wapi:loadProject", async () => {
  const result = await dialog.showOpenDialog({
    title: "Load project folder",
    properties: ["openDirectory"]
  });

  if (result.canceled || result.filePaths.length === 0) return null;
  const rootPath = result.filePaths[0];
  return { rootPath, files: await collectProjectFiles(rootPath) };
});

ipcMain.handle("window:minimize", (event) => {
  BrowserWindow.fromWebContents(event.sender)?.minimize();
});

ipcMain.handle("window:toggleMaximize", (event) => {
  const win = BrowserWindow.fromWebContents(event.sender);
  if (!win) return;
  if (win.isMaximized()) {
    win.unmaximize();
  } else {
    win.maximize();
  }
});

ipcMain.handle("window:close", (event) => {
  BrowserWindow.fromWebContents(event.sender)?.close();
});

ipcMain.handle("wapi:openFile", async () => {
  const result = await dialog.showOpenDialog({
    title: "Open Wapi script",
    filters: [
      { name: "Wapi scripts", extensions: ["wapi", "txt"] },
      { name: "All files", extensions: ["*"] }
    ],
    properties: ["openFile"]
  });

  if (result.canceled || result.filePaths.length === 0) return null;
  const filePath = result.filePaths[0];
  return readIdeFile(filePath);
});

ipcMain.handle("wapi:saveFile", async (_event, payload) => {
  let filePath = payload?.filePath;
  const source = typeof payload?.source === "string" ? payload.source : "";

  if (!filePath) {
    const result = await dialog.showSaveDialog({
      title: "Save Wapi script",
      defaultPath: "script.wapi",
      filters: [
        { name: "Wapi scripts", extensions: ["wapi"] },
        { name: "Text files", extensions: ["txt"] },
        { name: "All files", extensions: ["*"] }
      ]
    });

    if (result.canceled || !result.filePath) return null;
    filePath = result.filePath;
  }

  await fs.writeFile(filePath, source, "utf8");
  return { filePath };
});

app.whenReady().then(createWindow);

app.on("activate", () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow();
});

app.on("window-all-closed", () => {
  if (process.platform !== "darwin") app.quit();
});
