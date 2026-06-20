const { app, BrowserWindow, dialog, ipcMain } = require("electron");
const fs = require("node:fs/promises");
const path = require("node:path");
const { spawn } = require("node:child_process");
const pty = require("node-pty");

const isDev = Boolean(process.env.WAPI_IDE_DEV_SERVER_URL);
const appIcon = path.join(app.getAppPath(), "icon.ico");
const projectFileExtensions = new Set([".wapi", ".txt", ".json", ".cpp", ".c", ".h", ".hpp"]);
const shellCwdMarker = "__WAPI_CWD__";
const terminalSessions = new Map();
const executionSessions = new Map();
const executionOutputTruncated = "\n[WAPI_IDE] Output limit reached. Terminating Wapi process to protect memory.";

function defaultProjectConfig(name = "WapiProject") {
  return {
    name,
    entryFile: "main.wapi",
    defaultMode: "safe",
    strictPermissions: true,
    allowInjection: false,
    capabilities: ["proc.list", "proc.modules"]
  };
}

function terminalSessionKey(event, sessionId) {
  return `${event.sender.id}:${sessionId}`;
}

function executionSessionKey(event, command) {
  return `${event.sender.id}:${command}`;
}

function stopExecutionSession(key) {
  const child = executionSessions.get(key);
  if (child && !child.killed) child.kill();
  executionSessions.delete(key);
}

function emitTerminalData(webContents, payload) {
  if (!webContents.isDestroyed()) {
    webContents.send("wapi:terminal:data", payload);
  }
}

function lockDefaultZoom(webContents) {
  const resetZoom = () => webContents.setZoomLevel(0);

  resetZoom();
  webContents.on("did-finish-load", resetZoom);
  webContents.on("before-input-event", (event, input) => {
    if (!(input.control || input.meta)) return;
    if (!["+", "-", "=", "0"].includes(input.key)) return;
    event.preventDefault();
    resetZoom();
  });
}

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

  lockDefaultZoom(win.webContents);
  win.__wapiDirty = false;
  win.once("ready-to-show", () => win.show());
  win.on("close", (event) => {
    if (!win.__wapiDirty) return;
    const choice = dialog.showMessageBoxSync(win, {
      type: "warning",
      title: "Unsaved Wapi changes",
      message: "This project has unsaved changes.",
      detail: "Save your files before closing, or discard the changes.",
      buttons: ["Cancel", "Discard"],
      defaultId: 0,
      cancelId: 0
    });
    if (choice === 0) event.preventDefault();
  });

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

function projectConfigPath(rootPath) {
  return path.join(rootPath, ".wapi", "project.json");
}

async function readJsonFile(filePath, fallback = null) {
  try {
    return JSON.parse(await fs.readFile(filePath, "utf8"));
  } catch {
    return fallback;
  }
}

function normalizeProjectConfig(config = {}, fallbackName = "WapiProject") {
  const base = defaultProjectConfig(fallbackName);
  const next = { ...base, ...config };
  next.name = sanitizeProjectName(next.name || fallbackName);
  next.entryFile = safeRelativeProjectPath(next.entryFile || "main.wapi");
  if (next.defaultMode === "audit") next.defaultMode = "dev";
  next.defaultMode = ["safe", "dev", "unsafe"].includes(next.defaultMode) ? next.defaultMode : "safe";
  next.strictPermissions = Boolean(next.strictPermissions);
  next.allowInjection = Boolean(next.allowInjection);
  next.capabilities = Array.isArray(next.capabilities)
    ? [...new Set(next.capabilities.map((cap) => String(cap).trim()).filter(Boolean))]
    : base.capabilities;
  return next;
}

async function readProjectConfig(rootPath) {
  if (typeof rootPath !== "string" || !rootPath.trim()) return null;
  const config = await readJsonFile(projectConfigPath(rootPath), null);
  return config ? normalizeProjectConfig(config, path.basename(rootPath)) : null;
}

async function writeProjectConfig(rootPath, config) {
  if (typeof rootPath !== "string" || !rootPath.trim()) {
    throw new Error("Project root is required.");
  }
  const normalized = normalizeProjectConfig(config, path.basename(rootPath));
  const targetPath = projectConfigPath(rootPath);
  await fs.mkdir(path.dirname(targetPath), { recursive: true });
  await fs.writeFile(targetPath, `${JSON.stringify(normalized, null, 2)}\n`, "utf8");
  return normalized;
}

function recentProjectsPath() {
  return path.join(app.getPath("userData"), "recent-projects.json");
}

async function readRecentProjects() {
  const projects = await readJsonFile(recentProjectsPath(), []);
  return Array.isArray(projects)
    ? projects.filter((project) => typeof project.rootPath === "string" && project.rootPath.trim()).slice(0, 10)
    : [];
}

async function writeRecentProjects(projects) {
  const targetPath = recentProjectsPath();
  await fs.mkdir(path.dirname(targetPath), { recursive: true });
  await fs.writeFile(targetPath, `${JSON.stringify(projects.slice(0, 10), null, 2)}\n`, "utf8");
}

async function addRecentProject(rootPath) {
  if (typeof rootPath !== "string" || !rootPath.trim()) return readRecentProjects();
  const resolvedRoot = path.resolve(rootPath);
  const recent = await readRecentProjects();
  const next = [
    {
      rootPath: resolvedRoot,
      name: path.basename(resolvedRoot) || "WapiProject",
      openedAt: new Date().toISOString()
    },
    ...recent.filter((project) => path.resolve(project.rootPath) !== resolvedRoot)
  ];
  await writeRecentProjects(next);
  return next.slice(0, 10);
}

function sanitizeProjectName(name) {
  const cleaned = String(name || "WapiProject")
    .replace(/[<>:"/\\|?*\x00-\x1F]/g, "")
    .trim();
  return cleaned || "WapiProject";
}

async function uniqueProjectPath(parentPath, projectName) {
  const baseName = sanitizeProjectName(projectName);
  for (let index = 0; index < 50; index += 1) {
    const suffix = index === 0 ? "" : ` ${index + 1}`;
    const candidate = path.join(parentPath, `${baseName}${suffix}`);
    if (!(await fileExists(candidate))) return candidate;
  }
  throw new Error(`Could not create a unique folder for ${baseName}`);
}

function safeRelativeProjectPath(relativePath) {
  const normalized = path.normalize(String(relativePath || "")).replace(/^(\.\.(\\|\/|$))+/, "");
  const clean = normalized.replace(/^([\\\/])+/, "");
  return clean && clean !== "." ? clean : "main.wapi";
}

async function writeProjectFiles(rootPath, files) {
  for (const file of files) {
    const relativePath = safeRelativeProjectPath(file.relativePath || file.name);
    const targetPath = path.join(rootPath, relativePath);
    const resolvedRoot = path.resolve(rootPath);
    const resolvedTarget = path.resolve(targetPath);
    if (resolvedTarget !== resolvedRoot && !resolvedTarget.startsWith(`${resolvedRoot}${path.sep}`)) {
      throw new Error(`Invalid project file path: ${relativePath}`);
    }
    await fs.mkdir(path.dirname(targetPath), { recursive: true });
    await fs.writeFile(targetPath, typeof file.source === "string" ? file.source : "", "utf8");
  }
}

async function saveIdeFiles(files) {
  const results = [];
  for (const file of Array.isArray(files) ? files : []) {
    const filePath = typeof file?.filePath === "string" ? file.filePath : "";
    const source = typeof file?.source === "string" ? file.source : "";
    if (!filePath) {
      results.push({ ok: false, filePath: null, error: "Missing file path." });
      continue;
    }
    try {
      await fs.writeFile(filePath, source, "utf8");
      results.push({ ok: true, filePath });
    } catch (error) {
      results.push({ ok: false, filePath, error: error.message });
    }
  }
  return results;
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

function runProcess(exe, args, options = {}) {
  return new Promise((resolve) => {
    const child = spawn(exe, args, {
      cwd: path.dirname(exe),
      windowsHide: true
    });

    let stdout = "";
    let stderr = "";
    let outputBytes = 0;
    let killedForLimit = false;
    const maxOutputBytes = options.maxOutputBytes ?? 1024 * 1024;
    const timeout = setTimeout(() => {
      killedForLimit = true;
      stderr += `\n[WAPI_IDE] Timed out after ${Math.round((options.timeoutMs ?? 30000) / 1000)} seconds.`;
      child.kill();
    }, options.timeoutMs ?? 30000);

    if (options.sessionKey) executionSessions.set(options.sessionKey, child);

    function appendOutput(target, chunk) {
      const text = chunk.toString();
      outputBytes += Buffer.byteLength(text);
      if (outputBytes > maxOutputBytes) {
        if (!killedForLimit) {
          killedForLimit = true;
          stderr += executionOutputTruncated;
          child.kill();
        }
        return target;
      }
      return target + text;
    }

    child.stdout.on("data", (chunk) => {
      stdout = appendOutput(stdout, chunk);
    });
    child.stderr.on("data", (chunk) => {
      stderr = appendOutput(stderr, chunk);
    });
    child.on("error", (error) => {
      clearTimeout(timeout);
      if (options.sessionKey) executionSessions.delete(options.sessionKey);
      resolve({
        ok: false,
        code: null,
        stdout,
        stderr: error.message,
        exe
      });
    });
    child.on("close", (code) => {
      clearTimeout(timeout);
      if (options.sessionKey && executionSessions.get(options.sessionKey) === child) {
        executionSessions.delete(options.sessionKey);
      }
      resolve({
        ok: code === 0 && !killedForLimit,
        code,
        stdout,
        stderr,
        exe
      });
    });
  });
}

function stripShellMarker(stdout) {
  const lines = stdout.replace(/\r\n/g, "\n").split("\n");
  let cwd = null;
  const outputLines = [];

  for (const line of lines) {
    if (line.startsWith(shellCwdMarker)) {
      cwd = line.slice(shellCwdMarker.length).trim();
    } else {
      outputLines.push(line);
    }
  }

  return {
    cwd,
    stdout: outputLines.join("\n").replace(/\n+$/, "")
  };
}

async function resolveShellCwd(cwd) {
  const fallback = process.cwd();
  if (typeof cwd !== "string" || !cwd.trim()) return fallback;

  try {
    const stat = await fs.stat(cwd);
    return stat.isDirectory() ? cwd : fallback;
  } catch {
    return fallback;
  }
}

async function runShellCommand(payload = {}) {
  const shell = payload.shell === "cmd" ? "cmd" : "powershell";
  const command = typeof payload.command === "string" ? payload.command.trim() : "";
  const cwd = await resolveShellCwd(payload.cwd);

  if (!command) {
    return { ok: true, code: 0, stdout: "", stderr: "", cwd, shell };
  }

  const exe = shell === "cmd" ? "cmd.exe" : "powershell.exe";
  const powershellScript = [
    "$Error.Clear()",
    command,
    "$wapiExitCode = if ($global:LASTEXITCODE -ne $null) { $global:LASTEXITCODE } elseif ($Error.Count) { 1 } else { 0 }",
    `Write-Output "${shellCwdMarker}$((Get-Location).Path)"`,
    "exit $wapiExitCode"
  ].join("; ");
  const args = shell === "cmd"
    ? ["/v:on", "/d", "/s", "/c", `${command} & set __WAPI_EXIT__=!ERRORLEVEL! & echo ${shellCwdMarker}%CD% & exit /b !__WAPI_EXIT__`]
    : ["-NoLogo", "-NoProfile", "-EncodedCommand", Buffer.from(powershellScript, "utf16le").toString("base64")];

  return new Promise((resolve) => {
    const child = spawn(exe, args, {
      cwd,
      windowsHide: true
    });

    let stdout = "";
    let stderr = "";
    const timeout = setTimeout(() => {
      child.kill();
      stderr += "\nCommand timed out after 30 seconds.";
    }, 30000);

    child.stdout.on("data", (chunk) => {
      stdout += chunk.toString();
    });
    child.stderr.on("data", (chunk) => {
      stderr += chunk.toString();
    });
    child.on("error", (error) => {
      clearTimeout(timeout);
      resolve({
        ok: false,
        code: null,
        stdout: "",
        stderr: error.message,
        cwd,
        shell
      });
    });
    child.on("close", (code) => {
      clearTimeout(timeout);
      const parsed = stripShellMarker(stdout);
      resolve({
        ok: code === 0,
        code,
        stdout: parsed.stdout,
        stderr: stderr.trimEnd(),
        cwd: parsed.cwd || cwd,
        shell
      });
    });
  });
}

function stopTerminalSession(key) {
  const session = terminalSessions.get(key);
  if (!session) return;
  session.process.kill();
  terminalSessions.delete(key);
}

async function startTerminalSession(event, payload = {}) {
  const sessionId = typeof payload.sessionId === "string" ? payload.sessionId : "";
  if (!/^[a-zA-Z0-9_-]{1,80}$/.test(sessionId)) {
    return { ok: false, stderr: "Invalid terminal session ID." };
  }

  const key = terminalSessionKey(event, sessionId);
  stopTerminalSession(key);

  const shell = payload.shell === "cmd" ? "cmd" : "powershell";
  const cwd = await resolveShellCwd(payload.cwd);
  const exe = shell === "cmd" ? "cmd.exe" : "powershell.exe";
  const args = shell === "cmd"
    ? ["/d", "/q"]
    : ["-NoLogo", "-NoProfile"];
  const cols = Math.max(20, Math.min(500, Number(payload.cols) || 80));
  const rows = Math.max(5, Math.min(200, Number(payload.rows) || 24));

  const terminalProcess = pty.spawn(exe, args, {
    name: "xterm-256color",
    cols,
    rows,
    cwd,
    env: process.env,
    useConpty: true
  });

  const session = { process: terminalProcess, cwd, shell, sessionId };
  terminalSessions.set(key, session);

  terminalProcess.onData((data) => {
    emitTerminalData(event.sender, {
      type: "data",
      sessionId,
      shell,
      data
    });
  });
  terminalProcess.onExit(({ exitCode }) => {
    terminalSessions.delete(key);
    emitTerminalData(event.sender, {
      type: "exit",
      sessionId,
      shell,
      exitCode
    });
  });

  return { ok: true, sessionId, shell, cwd, pid: terminalProcess.pid };
}

function sendTerminalInput(event, payload = {}) {
  const sessionId = typeof payload.sessionId === "string" ? payload.sessionId : "";
  const key = terminalSessionKey(event, sessionId);
  const session = terminalSessions.get(key);
  const data = typeof payload.data === "string" ? payload.data : "";

  if (!session) {
    return { ok: false, stderr: "Terminal session is not running." };
  }

  session.process.write(data);
  return { ok: true };
}

function resizeTerminalSession(event, payload = {}) {
  const sessionId = typeof payload.sessionId === "string" ? payload.sessionId : "";
  const session = terminalSessions.get(terminalSessionKey(event, sessionId));
  if (!session) return { ok: false };

  const cols = Math.max(20, Math.min(500, Number(payload.cols) || 80));
  const rows = Math.max(5, Math.min(200, Number(payload.rows) || 24));
  session.process.resize(cols, rows);
  return { ok: true };
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

  const sessionKey = executionSessionKey(_event, command);
  stopExecutionSession(sessionKey);
  return runProcess(exe, buildArgs(command, source, options), {
    sessionKey,
    timeoutMs: command === "check" ? 8000 : 60000,
    maxOutputBytes: command === "check" ? 256 * 1024 : 1024 * 1024
  });
});

ipcMain.handle("wapi:locate", async () => resolveWapiExecutable());

ipcMain.handle("wapi:shell", async (_event, payload) => runShellCommand(payload));
ipcMain.handle("wapi:terminal:start", async (event, payload) => startTerminalSession(event, payload));
ipcMain.handle("wapi:terminal:send", (event, payload) => sendTerminalInput(event, payload));
ipcMain.handle("wapi:terminal:resize", (event, payload) => resizeTerminalSession(event, payload));
ipcMain.handle("wapi:terminal:stop", (event, payload = {}) => {
  stopTerminalSession(terminalSessionKey(event, payload.sessionId));
  return { ok: true };
});

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

ipcMain.handle("wapi:createProject", async (_event, payload = {}) => {
  const projectName = sanitizeProjectName(payload.name);
  const result = await dialog.showOpenDialog({
    title: "Choose where to create the project",
    message: `Select the folder that will contain "${projectName}".`,
    buttonLabel: "Create here",
    properties: ["openDirectory", "createDirectory"]
  });

  if (result.canceled || result.filePaths.length === 0) return null;

  const rootPath = await uniqueProjectPath(result.filePaths[0], projectName);
  const config = normalizeProjectConfig(payload.config, projectName);
  const files = Array.isArray(payload.files) && payload.files.length > 0
    ? payload.files
    : [{ name: "main.wapi", relativePath: "main.wapi", source: "listProcesses()\n" }];

  await fs.mkdir(rootPath, { recursive: false });
  await writeProjectFiles(rootPath, files);
  await writeProjectConfig(rootPath, config);
  await addRecentProject(rootPath);
  return { rootPath, config: await readProjectConfig(rootPath), files: await collectProjectFiles(rootPath) };
});

ipcMain.handle("wapi:loadProject", async (_event, requestedRootPath) => {
  let rootPath = typeof requestedRootPath === "string" ? requestedRootPath : "";
  if (rootPath) {
    const stat = await fs.stat(rootPath).catch(() => null);
    if (!stat?.isDirectory()) return null;
  } else {
    const result = await dialog.showOpenDialog({
      title: "Upload project folder",
      properties: ["openDirectory"]
    });

    if (result.canceled || result.filePaths.length === 0) return null;
    rootPath = result.filePaths[0];
  }
  await addRecentProject(rootPath);
  return { rootPath, config: await readProjectConfig(rootPath), files: await collectProjectFiles(rootPath) };
});

ipcMain.handle("wapi:readProjectConfig", async (_event, rootPath) => readProjectConfig(rootPath));

ipcMain.handle("wapi:writeProjectConfig", async (_event, rootPath, config) => writeProjectConfig(rootPath, config));

ipcMain.handle("wapi:listRecentProjects", async () => readRecentProjects());

ipcMain.handle("wapi:addRecentProject", async (_event, rootPath) => addRecentProject(rootPath));

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

ipcMain.handle("window:setDirtyState", (event, isDirty) => {
  const win = BrowserWindow.fromWebContents(event.sender);
  if (!win) return false;
  win.__wapiDirty = Boolean(isDirty);
  win.setTitle(`${win.__wapiDirty ? "* " : ""}Wapi IDE`);
  if (typeof win.setDocumentEdited === "function") win.setDocumentEdited(win.__wapiDirty);
  return true;
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

ipcMain.handle("wapi:saveFiles", async (_event, files) => saveIdeFiles(files));

app.whenReady().then(createWindow);

app.on("activate", () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow();
});

app.on("window-all-closed", () => {
  for (const key of terminalSessions.keys()) stopTerminalSession(key);
  for (const key of executionSessions.keys()) stopExecutionSession(key);
  if (process.platform !== "darwin") app.quit();
});
