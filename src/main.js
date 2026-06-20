import wapiIconUrl from "../wapi.png";
import evaluatorSource from "../evaluator.cpp?raw";
import bridge from "./tauri-bridge.js";
import "./styles.css";
import "./forge-ops.css";
import {
  Activity,
  ArrowRight,
  Braces,
  ChevronDown,
  ChevronRight,
  Cpu,
  Database,
  Ellipsis,
  FileCode,
  FileText,
  Folder,
  FolderOpen,
  FolderTree,
  Globe,
  ListTree,
  Maximize2,
  Minus,
  Play,
  Plus,
  Save,
  SaveAll,
  Search,
  Settings,
  ShieldCheck,
  SquareCheck,
  Terminal,
  Upload,
  X
} from "lucide";
import * as monaco from "monaco-editor/esm/vs/editor/editor.api";
import editorWorker from "monaco-editor/esm/vs/editor/editor.worker?worker";
import { Terminal as XtermTerminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";

self.MonacoEnvironment = {
  getWorker: () => new editorWorker()
};

function attrsString(attrs = {}) {
  return Object.entries(attrs)
    .filter(([, value]) => value !== undefined && value !== null)
    .map(([key, value]) => `${key}="${String(value)}"`)
    .join(" ");
}

function iconSvg(node, className = "ui-icon") {
  const children = node.map(([tag, attrs]) => `<${tag} ${attrsString(attrs)}></${tag}>`).join("");
  return `<svg class="${className}" viewBox="0 0 24 24" aria-hidden="true">${children}</svg>`;
}


const visibleExtensions = new Set([".wapi", ".txt", ".json", ".cpp", ".c", ".h", ".hpp"]);
const maxPanelLines = 300;
const welcomeTabId = "__welcome__";
const welcomeSource = "";

function escapeRegExp(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function normalizePath(value = "") {
  return String(value).replace(/\\/g, "/");
}

function fileExtension(path = "") {
  const match = String(path).toLowerCase().match(/\.[^.\\/]+$/);
  return match ? match[0] : "";
}

function fileName(path = "") {
  return normalizePath(path).split("/").filter(Boolean).pop() || "Untitled";
}

function parseCppParams(paramsSource) {
  const source = paramsSource.trim();
  if (!source || source === "void") return [];

  return source.split(",").map((part, index) => {
    const normalized = part.trim().replace(/\s+/g, " ");
    const nameMatch = normalized.match(/([a-zA-Z_]\w*)\s*(?:=[^=]*)?$/);
    if (!nameMatch) return { name: `arg${index + 1}`, type: "value" };

    const name = nameMatch[1];
    const type = normalized
      .slice(0, nameMatch.index)
      .replace(/\bconst\b/g, "")
      .replace(/\bstd::/g, "")
      .replace(/[&*]/g, "")
      .trim()
      .replace(/\s+/g, " ") || "value";
    return { name, type };
  });
}

function parseEvaluatorMethodParams(source) {
  const methods = new Map();
  const methodPattern = /WapiValue\s+Evaluator::(wapi_[a-zA-Z_]\w*)\s*\(([^)]*)\)/g;
  for (const match of source.matchAll(methodPattern)) {
    methods.set(match[1], parseCppParams(match[2]));
  }
  return methods;
}

function genericParams(count) {
  return Array.from({ length: count }, (_, index) => ({ name: `arg${index + 1}`, type: "value" }));
}

function parseEvaluatorFunctionRegistry(source) {
  const registryStart = source.indexOf("static const std::unordered_map<std::string, FunctionBinding> functions");
  const registryEnd = registryStart === -1 ? -1 : source.indexOf("\n    auto found", registryStart);
  const registrySource = registryStart === -1 || registryEnd === -1
    ? source
    : source.slice(registryStart, registryEnd);
  const methods = parseEvaluatorMethodParams(source);
  const entryPattern = /\{\s*"([a-zA-Z_][\w.]*)"\s*,\s*\{\s*(\d+)\s*,\s*"([^"]+)"\s*,\s*(true|false)\s*,/g;
  const entries = [...registrySource.matchAll(entryPattern)];

  const functions = entries.map((entry, index) => {
    const nextEntry = entries[index + 1];
    const body = registrySource.slice(entry.index, nextEntry?.index ?? registrySource.length);
    const methodName = body.match(/evaluator\.(wapi_[a-zA-Z_]\w*)\s*\(/)?.[1] ?? "";
    const argCount = Number(entry[2]);
    const params = (methods.get(methodName) ?? genericParams(argCount)).slice(0, argCount);

    return {
      name: entry[1],
      argCount,
      capability: entry[3],
      requiresInjectionFlag: entry[4] === "true",
      params: params.length === argCount ? params : genericParams(argCount)
    };
  });

  const byName = new Map(functions.map((fn) => [fn.name, fn]));
  const aliasPattern = /\{\s*"([a-zA-Z_][\w.]*)"\s*,\s*"([a-zA-Z_]\w*)"\s*\}/g;
  for (const alias of registrySource.matchAll(aliasPattern)) {
    const target = byName.get(alias[2]);
    if (!target || byName.has(alias[1])) continue;
    const aliasFn = { ...target, name: alias[1] };
    functions.push(aliasFn);
    byName.set(aliasFn.name, aliasFn);
  }

  return functions.sort((a, b) => a.name.localeCompare(b.name));
}

const wapiRuntimeFunctions = parseEvaluatorFunctionRegistry(evaluatorSource);
const wapiRuntimeFunctionMap = new Map(wapiRuntimeFunctions.map((fn) => [fn.name, fn]));
const wapiFunctionNameRegex = wapiRuntimeFunctions.length
  ? new RegExp(`\\b(?:${wapiRuntimeFunctions.map((fn) => escapeRegExp(fn.name)).join("|")})\\b`)
  : /(?!)/;
const runtimeCapabilities = [...new Set(wapiRuntimeFunctions.map((fn) => fn.capability))].sort();

function wapiFunctionSignature(fn) {
  const params = fn.params.map((param) => `${param.name}: ${param.type}`).join(", ");
  return `${fn.name}(${params})`;
}

function wapiFunctionSnippet(fn) {
  if (fn.params.length === 0) return `${fn.name}()`;
  const params = fn.params
    .map((param, index) => "${" + `${index + 1}:${param.name}` + "}")
    .join(", ");
  return `${fn.name}(${params})`;
}

function wapiFunctionDocs(fn) {
  return [
    `Capability: ${fn.capability}`,
    `Arguments: ${fn.argCount}`,
    fn.requiresInjectionFlag ? "Requires allow injection outside unsafe mode." : "No injection flag required."
  ].join("\n");
}

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

function normalizeProjectConfig(config = {}, fallbackName = "WapiProject") {
  const next = { ...defaultProjectConfig(fallbackName), ...(config ?? {}) };
  next.name = String(next.name || fallbackName).trim() || fallbackName;
  next.entryFile = normalizePath(next.entryFile || "main.wapi").replace(/^\/+/, "") || "main.wapi";
  if (next.defaultMode === "audit") next.defaultMode = "dev";
  next.defaultMode = ["safe", "dev", "unsafe"].includes(next.defaultMode) ? next.defaultMode : "safe";
  next.strictPermissions = Boolean(next.strictPermissions);
  next.allowInjection = Boolean(next.allowInjection);
  next.capabilities = Array.isArray(next.capabilities)
    ? [...new Set(next.capabilities.map((cap) => String(cap).trim()).filter(Boolean))]
    : defaultProjectConfig(fallbackName).capabilities;
  return next;
}

function templateConfig(name, templateId) {
  const base = defaultProjectConfig(name);
  if (templateId === "process-inspector") {
    return {
      ...base,
      capabilities: ["proc.list", "proc.modules"]
    };
  }
  if (templateId === "memory-sandbox") {
    return {
      ...base,
      strictPermissions: true,
      allowInjection: false,
      capabilities: [
        "proc.list",
        "proc.open.all_access",
        "mem.alloc",
        "mem.write",
        "mem.read",
        "mem.free",
        "proc.handle.close"
      ]
    };
  }
  return { ...base, capabilities: ["proc.list"] };
}

const projectTemplates = [
  {
    id: "empty",
    name: "Empty",
    note: "A clean Wapi entry file with check-first defaults.",
    source: (name) => [
      `// ${name}`,
      "// Check before running runtime actions.",
      "listProcesses()",
      ""
    ].join("\n")
  },
  {
    id: "process-inspector",
    name: "Process Inspector",
    note: "Process discovery plus module inspection APIs.",
    source: (name) => [
      `// ${name} - Process Inspector`,
      "int pid = findProcessPID(\"notepad\")",
      "listModules(pid)",
      "int base = getModuleBaseAddress(pid, \"kernel32.dll\")",
      ""
    ].join("\n")
  },
  {
    id: "memory-sandbox",
    name: "Memory Sandbox",
    note: "Memory allocation/read/write flow using allocMemory.",
    source: (name) => [
      `// ${name} - Memory Sandbox`,
      "int pid = findProcessPID(\"notepad\")",
      "int handle = openProcess(pid)",
      "int address = allocMemory(handle, 64)",
      "writeMemory(handle, address, 1234)",
      "int value = readMemory(handle, address)",
      "freeMemory(handle, address)",
      "closeHandle(handle)",
      ""
    ].join("\n")
  }
];

const ideState = {
  files: [],
  activeFileId: welcomeTabId,
  welcomeTabOpen: true,
  openFileIds: [],
  collapsedFolders: new Set(),
  projectRoot: null,
  projectConfig: defaultProjectConfig(),
  searchQuery: "",
  recentProjects: [],
  activePanel: "explorer",
  activeTool: "output",
  menuOpen: false,
  projectDialogOpen: false,
  selectedTemplate: "empty",
  dirty: false,
  outputLines: [],
  auditLines: [],
  problems: [],
  terminalTabs: [],
  activeTerminalId: null,
  inspectorOpen: false,
  runtimeInspector: {
    process: null,
    lastResult: null,
    events: [{ time: new Date().toLocaleTimeString([], { hour12: false }), message: "Runtime inspector standing by", status: "idle" }]
  }
};

const terminalRuntimes = new Map();
let terminalSequence = 0;

const editorState = {
  editor: null,
  model: null,
  changeDisposable: null,
  cursorDisposable: null,
  diagnosticsTimer: null,
  diagnosticsToken: 0
};

function runtimeOptions() {
  return {
    mode: ideState.projectConfig.defaultMode,
    strictPermissions: ideState.projectConfig.strictPermissions,
    allowInjection: ideState.projectConfig.allowInjection,
    capabilities: ideState.projectConfig.capabilities
  };
}

function languageForFile(file) {
  const path = (file?.name || file?.relativePath || "").toLowerCase();
  if (path.endsWith(".cpp") || path.endsWith(".c") || path.endsWith(".h") || path.endsWith(".hpp")) return "cpp";
  if (path.endsWith(".json")) return "json";
  if (path.endsWith(".txt")) return "plaintext";
  return "wapi";
}

function normalizeProjectFile(file = {}) {
  const filePath = typeof file.filePath === "string" ? file.filePath : "";
  const relativePath = normalizePath(
    typeof file.relativePath === "string" && file.relativePath
      ? file.relativePath
      : filePath
        ? fileName(filePath)
        : file.name || "Untitled.wapi"
  );
  const name = file.name || fileName(relativePath);
  const id = filePath || relativePath || name;
  const source = typeof file.source === "string" ? file.source : "";
  const originalSource = typeof file.originalSource === "string" ? file.originalSource : source;
  return {
    ...file,
    id,
    filePath,
    name,
    relativePath,
    source,
    originalSource,
    dirty: Boolean(file.dirty) || source !== originalSource
  };
}

function visibleProjectFiles(files = []) {
  return files
    .map(normalizeProjectFile)
    .filter((file) => visibleExtensions.has(fileExtension(file.relativePath || file.name)))
    .sort((a, b) => a.relativePath.localeCompare(b.relativePath));
}

function activeFile() {
  if (ideState.activeFileId === welcomeTabId) return null;
  return ideState.files.find((file) => file.id === ideState.activeFileId) ?? null;
}

function isWelcomeActive() {
  return ideState.welcomeTabOpen && ideState.activeFileId === welcomeTabId;
}

function isDirty(file) {
  return Boolean(file?.dirty) || (file ? file.source !== file.originalSource : false);
}

function projectDisplayName() {
  if (ideState.projectConfig?.name) return ideState.projectConfig.name;
  if (ideState.projectRoot) return fileName(ideState.projectRoot);
  return "WapiProject";
}

function relativePathForFilePath(filePath) {
  const normalized = normalizePath(filePath);
  const root = normalizePath(ideState.projectRoot || "");
  if (root && normalized.toLowerCase().startsWith(`${root.toLowerCase()}/`)) {
    return normalized.slice(root.length + 1);
  }
  return fileName(filePath);
}

function starterProjectFiles(projectName = "WapiProject", templateId = "empty") {
  const template = projectTemplates.find((item) => item.id === templateId) ?? projectTemplates[0];
  const config = templateConfig(projectName, template.id);
  return [
    {
      name: "main.wapi",
      relativePath: "main.wapi",
      source: template.source(projectName)
    },
    {
      name: "project.json",
      relativePath: ".wapi/project.json",
      source: `${JSON.stringify(config, null, 2)}\n`
    },
    {
      name: "README.txt",
      relativePath: "README.txt",
      source: [
        projectName,
        "",
        `Template: ${template.name}`,
        "Use Check before Run. Runtime settings live in .wapi/project.json.",
        ""
      ].join("\n")
    }
  ];
}

function mergeProjectFiles(existingFiles, incomingFiles) {
  const filesById = new Map(existingFiles.map((file) => [file.id, file]));
  for (const file of visibleProjectFiles(incomingFiles)) {
    filesById.set(file.id, file);
  }
  return [...filesById.values()].sort((a, b) => a.relativePath.localeCompare(b.relativePath));
}

function buildProjectTree(files) {
  const root = { name: "", path: "", folders: new Map(), files: [] };
  for (const file of files) {
    const parts = file.relativePath.split("/").filter(Boolean);
    let node = root;
    parts.slice(0, -1).forEach((part, index) => {
      const folderPath = parts.slice(0, index + 1).join("/");
      if (!node.folders.has(part)) node.folders.set(part, { name: part, path: folderPath, folders: new Map(), files: [] });
      node = node.folders.get(part);
    });
    node.files.push(file);
  }
  return root;
}

function filteredTreeFiles() {
  const query = ideState.searchQuery.trim().toLowerCase();
  if (!query || ideState.activePanel !== "explorer") return ideState.files;
  return ideState.files.filter((file) => `${file.name} ${file.relativePath}`.toLowerCase().includes(query));
}

function searchResults() {
  const query = ideState.searchQuery.trim().toLowerCase();
  if (!query) return [];
  const results = [];
  for (const file of ideState.files) {
    const lines = file.source.replace(/\r\n/g, "\n").split("\n");
    lines.forEach((line, index) => {
      const column = line.toLowerCase().indexOf(query);
      if (column !== -1) {
        results.push({
          file,
          line: index + 1,
          column: column + 1,
          text: line.trim() || "(blank line)"
        });
      }
    });
  }
  return results.slice(0, 120);
}

function outlineEntries(file = activeFile()) {
  if (!file) return [];
  const entries = [];
  const lines = file.source.replace(/\r\n/g, "\n").split("\n");
  lines.forEach((line, index) => {
    const declaration = line.match(/^\s*(?:int|string|bool|float|double|long)\s+([a-zA-Z_]\w*)/);
    if (declaration) {
      entries.push({ type: "variable", label: declaration[1], line: index + 1 });
    }
    for (const match of line.matchAll(/\b([a-zA-Z_]\w*)\s*\(/g)) {
      const fn = wapiRuntimeFunctionMap.get(match[1]);
      if (fn) entries.push({ type: "function", label: fn.name, line: index + 1, capability: fn.capability });
    }
  });
  return entries.slice(0, 100);
}

function splitLines(value = "") {
  return String(value).replace(/\r\n/g, "\n").split("\n").filter((line) => line.length > 0);
}

function recordRuntimeEvent(message, status = "info") {
  ideState.runtimeInspector.events.unshift({
    time: new Date().toLocaleTimeString([], { hour12: false }),
    message,
    status
  });
  ideState.runtimeInspector.events = ideState.runtimeInspector.events.slice(0, 8);
  renderRuntimeInspector();
}

function recordRuntimeResult(command, result, file) {
  ideState.runtimeInspector.lastResult = result;
  ideState.runtimeInspector.process = {
    pid: result.pid ?? null,
    name: result.exe ? fileName(result.exe) : "Wapi.exe",
    path: result.exe ?? "Executable not located",
    command,
    file: file?.relativePath ?? "main.wapi",
    duration: result.durationMs ?? 0,
    status: result.ok ? "Completed" : result.timedOut ? "Timed out" : "Failed"
  };
  recordRuntimeEvent(
    `${command === "check" ? "Check" : "Run"} ${result.ok ? "completed" : "failed"} for ${file?.relativePath ?? "script"}`,
    result.ok ? "success" : "error"
  );
}

function renderRuntimeInspector() {
  const drawer = document.getElementById("runtimeInspectorDrawer");
  const toggle = document.getElementById("toolbarInspector");
  if (!drawer || !toggle) return;

  drawer.classList.toggle("is-open", ideState.inspectorOpen);
  drawer.setAttribute("aria-hidden", String(!ideState.inspectorOpen));
  toggle.classList.toggle("is-active", ideState.inspectorOpen);
  toggle.setAttribute("aria-expanded", String(ideState.inspectorOpen));

  const process = ideState.runtimeInspector.process;
  const processBody = drawer.querySelector("[data-inspector-process]");
  if (processBody) {
    const rows = process
      ? [
          ["PID", process.pid ?? "--"],
          ["NAME", process.name],
          ["SOURCE", process.file],
          ["STATUS", process.status],
          ["DURATION", `${process.duration} ms`]
        ]
      : [["STATE", "No runtime process yet"], ["SOURCE", "Run Check or Run to inspect"]];
    processBody.innerHTML = rows.map(([label, value]) =>
      `<div class="inspector-kv"><span>${label}</span><strong>${String(value)}</strong></div>`
    ).join("");
  }

  const capabilityBody = drawer.querySelector("[data-inspector-capabilities]");
  if (capabilityBody) {
    const caps = ideState.projectConfig.capabilities.length
      ? ideState.projectConfig.capabilities
      : ["No capabilities granted"];
    capabilityBody.innerHTML = caps.slice(0, 5).map((capability) =>
      `<div class="inspector-cap"><span class="inspector-led"></span><code>${capability}</code><strong>${capability.startsWith("No ") ? "NONE" : "GRANTED"}</strong></div>`
    ).join("");
  }

  const mapBody = drawer.querySelector("[data-inspector-memory]");
  if (mapBody) {
    const last = ideState.runtimeInspector.lastResult;
    mapBody.innerHTML = last?.exe
      ? `<div class="memory-row memory-head"><span>MODULE</span><span>STATE</span><span>HOST</span></div>
         <div class="memory-row"><span>${fileName(last.exe)}</span><span>managed</span><span>${last.pid ?? "--"}</span></div>`
      : '<div class="inspector-empty">Memory map becomes available with a runtime session.</div>';
  }

  const eventsBody = drawer.querySelector("[data-inspector-events]");
  if (eventsBody) {
    eventsBody.innerHTML = ideState.runtimeInspector.events.map((event) =>
      `<div class="event-row is-${event.status}"><time>${event.time}</time><span>${event.message}</span></div>`
    ).join("");
  }
}

function appendLines(target, lines, kind = "info") {
  for (const line of lines) target.push({ text: line, kind, at: new Date().toLocaleTimeString() });
  if (target.length > maxPanelLines) target.splice(0, target.length - maxPanelLines);
}

function setStatus(message, timeout = 1800) {
  const node = document.getElementById("statusInstance");
  if (!node) return;
  node.textContent = message;
  node.classList.remove("is-live");
  node.offsetWidth;
  node.classList.add("is-live");
  window.clearTimeout(setStatus.timeoutId);
  setStatus.timeoutId = window.setTimeout(() => {
    node.textContent = ideState.dirty ? "Unsaved" : "Ready";
    node.classList.remove("is-live");
  }, timeout);
}

function updateCursorStatus() {
  const cursor = document.getElementById("statusCursor");
  if (!cursor || !editorState.editor) return;
  const position = editorState.editor.getPosition();
  cursor.textContent = position ? `Ln ${position.lineNumber}, Col ${position.column}` : "Ln 1, Col 1";
}

function syncDirtyState(render = true) {
  ideState.dirty = ideState.files.some(isDirty);
  bridge.window.setDirtyState(ideState.dirty);
  const statusDirty = document.getElementById("statusDirty");
  if (statusDirty) {
    statusDirty.textContent = ideState.dirty ? "Unsaved changes" : "Saved";
    statusDirty.classList.toggle("is-dirty", ideState.dirty);
  }
  if (render) {
    renderDocumentTabs();
    renderSidePanel();
  }
}

function confirmDiscardUnsaved() {
  if (!ideState.dirty) return true;
  return window.confirm("This project has unsaved changes. Discard them?");
}

async function persistProjectConfig() {
  if (!ideState.projectRoot) return;
  await bridge.writeProjectConfig(ideState.projectRoot, ideState.projectConfig);
  const configFile = ideState.files.find((file) => normalizePath(file.relativePath) === ".wapi/project.json");
  if (configFile) {
    configFile.source = `${JSON.stringify(ideState.projectConfig, null, 2)}\n`;
    configFile.originalSource = configFile.source;
    configFile.dirty = false;
    if (activeFile()?.id === configFile.id) setEditorModel(configFile, true);
  }
  syncDirtyState();
}

async function replaceProject(project, message = "Project loaded") {
  let config = project.config ?? null;
  if (!config && project.rootPath) config = await bridge.readProjectConfig(project.rootPath);
  config = normalizeProjectConfig(config, project.rootPath ? fileName(project.rootPath) : "WapiProject");

  ideState.files = visibleProjectFiles(project.files ?? []);
  ideState.projectRoot = project.rootPath ?? null;
  ideState.projectConfig = config;
  ideState.openFileIds = [];
  ideState.activeFileId = ideState.welcomeTabOpen ? welcomeTabId : null;
  ideState.searchQuery = "";
  ideState.collapsedFolders = new Set();
  ideState.problems = [];
  ideState.outputLines = [];
  ideState.auditLines = [];
  ideState.activePanel = "explorer";
  ideState.activeTool = "output";

  const entryFile = ideState.files.find((file) => normalizePath(file.relativePath) === normalizePath(config.entryFile));
  const firstWapi = ideState.files.find((file) => file.relativePath.endsWith(".wapi"));
  const firstFile = entryFile ?? firstWapi ?? ideState.files[0] ?? null;
  if (firstFile) openFileInEditor(firstFile.id, false);

  if (ideState.projectRoot) {
    ideState.recentProjects = await bridge.addRecentProject(ideState.projectRoot);
  }

  renderAll();
  setEditorModel(activeFile());
  syncDirtyState();
  setStatus(message);
}

function openFileInEditor(fileId, render = true) {
  const file = ideState.files.find((item) => item.id === fileId);
  if (!file) return;
  ideState.activeFileId = file.id;
  if (!ideState.openFileIds.includes(file.id)) ideState.openFileIds.push(file.id);
  if (render) {
    renderAll();
    setEditorModel(file);
  }
}

function closeDocumentTab(fileId) {
  if (fileId === welcomeTabId) {
    ideState.welcomeTabOpen = false;
    if (ideState.activeFileId === welcomeTabId) {
      ideState.activeFileId = ideState.openFileIds.at(-1) ?? null;
    }
    renderAll();
    setEditorModel(activeFile());
    return;
  }
  ideState.openFileIds = ideState.openFileIds.filter((id) => id !== fileId);
  if (ideState.activeFileId === fileId) {
    ideState.activeFileId = ideState.openFileIds.at(-1) ?? (ideState.welcomeTabOpen ? welcomeTabId : null);
  }
  renderAll();
  setEditorModel(activeFile());
}

function markerProblemsForModel(model = editorState.model) {
  if (!model) return [];
  const file = activeFile();
  if (!file) return [];
  return ideState.problems
    .filter((problem) => problem.fileId === file.id)
    .map((problem) => ({
      severity: problem.severity === "warning" ? monaco.MarkerSeverity.Warning : monaco.MarkerSeverity.Error,
      message: problem.message,
      startLineNumber: problem.line,
      startColumn: problem.column,
      endLineNumber: problem.line,
      endColumn: problem.wholeFile ? model.getLineMaxColumn(problem.line) : Math.max(problem.column + 1, problem.column),
      source: "wapi check"
    }));
}

function applyMarkers() {
  if (!editorState.model) return;
  monaco.editor.setModelMarkers(editorState.model, "wapi", markerProblemsForModel(editorState.model));
}

function inferProblemLocation(text) {
  const match = text.match(/\b(?:line|Line)\s+(\d+)(?:\D+(?:column|col|Col)\s+(\d+))?/);
  return {
    line: match ? Math.max(1, Number(match[1])) : 1,
    column: match?.[2] ? Math.max(1, Number(match[2])) : 1,
    wholeFile: !match
  };
}

function problemsFromResult(result, file = activeFile()) {
  if (!file) return [];
  const errorLines = [
    ...splitLines(result.stderr),
    ...splitLines(result.stdout).filter((line) => /\b(error|exception|E_[A-Z_]+|failed)\b/i.test(line))
  ];
  if (result.ok && errorLines.length === 0) return [];
  const lines = errorLines.length ? errorLines : [`Wapi ${result.code === null ? "process" : "check"} failed.`];
  return lines.map((line) => {
    const location = inferProblemLocation(line);
    return {
      fileId: file.id,
      fileName: file.name,
      relativePath: file.relativePath,
      message: line,
      severity: "error",
      ...location
    };
  });
}

function commandTargetFile(command) {
  const active = activeFile();
  const entryPath = normalizePath(ideState.projectConfig.entryFile || "main.wapi");
  const entry = ideState.files.find((file) => normalizePath(file.relativePath) === entryPath);
  if (entry && ["check", "run"].includes(command)) return entry;
  if (active && languageForFile(active) === "wapi") return active;
  return ideState.files.find((file) => languageForFile(file) === "wapi") ?? null;
}

function sourceForCommandFile(file) {
  const active = activeFile();
  if (file?.id === active?.id && editorState.model) {
    file.source = editorState.model.getValue();
  }
  return file?.source ?? "";
}

async function runWapiCommand(command, { silent = false } = {}) {
  const active = activeFile();
  const file = silent && active && languageForFile(active) === "wapi"
    ? active
    : commandTargetFile(command);
  if (!file) {
    if (!silent) {
      ideState.activeTool = "output";
      appendLines(ideState.outputLines, ["No Wapi entry file found. Create or open a .wapi file first."], "error");
      renderToolWindow();
    }
    setStatus("No Wapi entry file");
    return null;
  }

  const token = ++editorState.diagnosticsToken;
  const source = sourceForCommandFile(file);
  file.source = source;
  if (!silent) {
    ideState.activeTool = "output";
    if (command === "run") ideState.outputLines = [];
    appendLines(ideState.outputLines, [`> wapi.exe ${command} ${file.relativePath}`], "command");
    appendLines(ideState.outputLines, [`Starting ${command} with mode=${runtimeOptions().mode}, strict=${runtimeOptions().strictPermissions ? "on" : "off"}`], "info");
    renderToolWindow();
  }

  if (!silent) recordRuntimeEvent(`${command === "check" ? "Check" : "Run"} started for ${file.relativePath}`, "pending");
  const result = await bridge.execute({ command, source, options: runtimeOptions() });
  if (!silent) recordRuntimeResult(command, result, file);
  if (silent && token !== editorState.diagnosticsToken) return result;

  const stdoutLines = splitLines(result.stdout);
  const stderrLines = splitLines(result.stderr);
  const auditLines = stdoutLines.filter((line) => line.includes("[WAPI_AUDIT]"));

  if (!silent) {
    appendLines(ideState.outputLines, stdoutLines, result.ok ? "info" : "error");
    appendLines(ideState.outputLines, stderrLines, "error");
    if (stdoutLines.length === 0 && stderrLines.length === 0) {
      appendLines(ideState.outputLines, ["No stdout/stderr. The script ran, but the selected functions did not print output."], "warning");
    }
    appendLines(ideState.outputLines, [`Process ${result.ok ? "completed" : "failed"} with code ${result.code ?? "unknown"}`], result.ok ? "success" : "error");
    if (auditLines.length) {
      appendLines(ideState.auditLines, auditLines, "audit");
    }
  }

  ideState.problems = problemsFromResult(result, file);
  applyMarkers();
  renderToolWindow();
  setStatus(result.ok && ideState.problems.length === 0 ? `${command === "check" ? "Check" : "Run"} passed` : `${command === "check" ? "Check" : "Run"} found issues`);
  return result;
}

function scheduleDiagnostics() {
  window.clearTimeout(editorState.diagnosticsTimer);
  const file = activeFile();
  if (!file || languageForFile(file) !== "wapi") return;
  editorState.diagnosticsTimer = window.setTimeout(() => runWapiCommand("check", { silent: true }), 850);
}

function formatWapi(source) {
  return source
    .replace(/\r\n/g, "\n")
    .split("\n")
    .map((line) => line.trimEnd())
    .join("\n")
    .replace(/\n{3,}/g, "\n\n")
    .trimEnd() + "\n";
}

function installMonacoLanguage() {
  monaco.languages.register({ id: "wapi" });
  monaco.languages.setMonarchTokensProvider("wapi", {
    tokenizer: {
      root: [
        [/\/\/.*$/, "comment"],
        [/"([^"\\]|\\.)*$/, "string.invalid"],
        [/"/, "string", "@string"],
        [/\b(?:print|let|if|else|while|for|return|true|false|null|check|run|int|string|bool|long)\b/, "keyword"],
        [wapiFunctionNameRegex, "type.identifier"],
        [/\b0x[0-9a-fA-F]+\b/, "number.hex"],
        [/\b\d+(?:\.\d+)?\b/, "number"],
        [/[{}()[\]]/, "@brackets"],
        [/[a-zA-Z_]\w*/, "identifier"]
      ],
      string: [
        [/[^\\"]+/, "string"],
        [/\\./, "string.escape"],
        [/"/, "string", "@pop"]
      ]
    }
  });
  monaco.languages.setLanguageConfiguration("wapi", {
    comments: { lineComment: "//" },
    brackets: [["{", "}"], ["[", "]"], ["(", ")"]],
    autoClosingPairs: [
      { open: "{", close: "}" },
      { open: "[", close: "]" },
      { open: "(", close: ")" },
      { open: '"', close: '"' }
    ]
  });
  monaco.languages.registerCompletionItemProvider("wapi", {
    provideCompletionItems(model, position) {
      const word = model.getWordUntilPosition(position);
      const range = {
        startLineNumber: position.lineNumber,
        endLineNumber: position.lineNumber,
        startColumn: word.startColumn,
        endColumn: word.endColumn
      };

      return {
        suggestions: wapiRuntimeFunctions.map((fn) => ({
          label: fn.name,
          kind: monaco.languages.CompletionItemKind.Function,
          detail: wapiFunctionSignature(fn),
          documentation: wapiFunctionDocs(fn),
          insertText: wapiFunctionSnippet(fn),
          insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
          range,
          sortText: `0_${fn.name}`
        }))
      };
    }
  });
  monaco.languages.registerHoverProvider("wapi", {
    provideHover(model, position) {
      const word = model.getWordAtPosition(position);
      const fn = word ? wapiRuntimeFunctionMap.get(word.word) : null;
      if (!fn) return null;
      return {
        range: new monaco.Range(position.lineNumber, word.startColumn, position.lineNumber, word.endColumn),
        contents: [
          { value: "```wapi\n" + wapiFunctionSignature(fn) + "\n```" },
          { value: wapiFunctionDocs(fn).replace(/\n/g, "  \n") }
        ]
      };
    }
  });
  monaco.languages.registerSignatureHelpProvider("wapi", {
    signatureHelpTriggerCharacters: ["(", ","],
    provideSignatureHelp(model, position) {
      const linePrefix = model.getValueInRange({
        startLineNumber: position.lineNumber,
        startColumn: 1,
        endLineNumber: position.lineNumber,
        endColumn: position.column
      });
      const match = [...linePrefix.matchAll(/([a-zA-Z_]\w*(?:\.[a-zA-Z_]\w*)*)\s*\(([^()]*)$/g)].pop();
      const fn = match ? wapiRuntimeFunctionMap.get(match[1]) : null;
      if (!fn) return null;
      const activeParameter = match[2].trim() ? Math.min(match[2].split(",").length - 1, Math.max(fn.params.length - 1, 0)) : 0;
      return {
        value: {
          signatures: [{
            label: wapiFunctionSignature(fn),
            documentation: wapiFunctionDocs(fn),
            parameters: fn.params.map((param) => ({
              label: param.name,
              documentation: param.type
            }))
          }],
          activeSignature: 0,
          activeParameter
        },
        dispose() {}
      };
    }
  });
  monaco.languages.registerDocumentFormattingEditProvider("wapi", {
    provideDocumentFormattingEdits(model) {
      return [{ range: model.getFullModelRange(), text: formatWapi(model.getValue()) }];
    }
  });
  monaco.editor.defineTheme("wapi-dark", {
    base: "vs-dark",
    inherit: true,
    rules: [
      { token: "comment", foreground: "444444", fontStyle: "italic" },
      { token: "keyword", foreground: "aaaaaa", fontStyle: "bold" },
      { token: "type.identifier", foreground: "999999" },
      { token: "string", foreground: "888888" },
      { token: "number", foreground: "999999" },
      { token: "delimiter", foreground: "666666" },
      { token: "operator", foreground: "777777" }
    ],
    colors: {
      "editor.background": "#1a1a1a",
      "editor.foreground": "#888888",
      "editorLineNumber.foreground": "#333333",
      "editorLineNumber.activeForeground": "#22c55e",
      "editorCursor.foreground": "#22c55e",
      "editor.selectionBackground": "#2a2a2a88",
      "editor.inactiveSelectionBackground": "#22222266",
      "editor.selectionHighlightBackground": "#22222244",
      "editor.lineHighlightBackground": "#1f1f1f",
      "editorIndentGuide.background1": "#222222",
      "editorIndentGuide.activeBackground1": "#333333",
      "editorBracketMatch.background": "#22c55e22",
      "editorBracketMatch.border": "#22c55e66",
      "scrollbarSlider.background": "#2a2a2a44",
      "scrollbarSlider.hoverBackground": "#33333366",
      "scrollbarSlider.activeBackground": "#44444488",
      "editorWidget.background": "#1e1e1e",
      "editorWidget.border": "#2a2a2a",
      "editorSuggestWidget.background": "#1e1e1e",
      "editorSuggestWidget.border": "#2a2a2a",
      "editorSuggestWidget.selectedBackground": "#2a2a2a",
      "input.background": "#161616",
      "input.border": "#2a2a2a",
      "focusBorder": "#22c55e",
      "minimap.background": "#161616"
    }
  });
}

function setEditorModel(file, forceRefresh = false) {
  if (!editorState.editor) return;
  const source = file?.source ?? welcomeSource;
  const language = languageForFile(file);
  const uri = monaco.Uri.parse(`wapi://workspace/${encodeURIComponent(file?.id ?? "welcome.wapi")}`);
  const existing = monaco.editor.getModel(uri);
  const model = existing ?? monaco.editor.createModel(source, language, uri);

  editorState.changeDisposable?.dispose();
  if ((forceRefresh || !existing) && model.getValue() !== source) {
    model.setValue(source);
  }
  monaco.editor.setModelLanguage(model, language);
  editorState.model = model;
  editorState.editor.setModel(model);
  editorState.editor.updateOptions({ readOnly: !file });
  editorState.changeDisposable = model.onDidChangeContent(() => {
    const active = activeFile();
    if (!active || model !== editorState.model) return;
    active.source = model.getValue();
    active.dirty = active.source !== active.originalSource;
    syncDirtyState();
    updateStatusLine();
    renderDocumentTabs();
    scheduleDiagnostics();
  });
  updateStatusLine();
  updateCursorStatus();
  applyMarkers();
}

function createMonacoEditor() {
  const container = document.getElementById("monacoEditor");
  if (!container || editorState.editor) return;

  editorState.editor = monaco.editor.create(container, {
    theme: "wapi-dark",
    automaticLayout: true,
    fontFamily: '"Cascadia Code", "SF Mono", Consolas, monospace',
    fontSize: 13,
    lineHeight: 20,
    fontLigatures: false,
    minimap: {
      enabled: true,
      side: "right",
      size: "proportional",
      showSlider: "mouseover",
      renderCharacters: false,
      maxColumn: 92
    },
    scrollBeyondLastLine: false,
    renderLineHighlight: "line",
    roundedSelection: false,
    cursorBlinking: "smooth",
    cursorSmoothCaretAnimation: "on",
    overviewRulerBorder: false,
    fixedOverflowWidgets: true,
    padding: { top: 14, bottom: 18 }
  });
  editorState.cursorDisposable?.dispose();
  editorState.cursorDisposable = editorState.editor.onDidChangeCursorPosition(updateCursorStatus);
  setEditorModel(activeFile());
}

function updateStatusLine() {
  const file = activeFile();
  const language = languageForFile(file);
  const displayLanguage = language === "cpp" ? "C++" : language === "plaintext" ? "Text" : language === "json" ? "JSON" : "Wapi";
  const statusFile = document.getElementById("statusFile");
  const statusLanguage = document.getElementById("statusLanguage");
  const editorStatus = document.getElementById("editorStatus");
  if (statusFile) {
    statusFile.textContent = file ? `${file.relativePath}${isDirty(file) ? " *" : ""}` : "Welcome";
    statusFile.title = file?.relativePath ?? "Welcome";
  }
  if (statusLanguage) statusLanguage.textContent = displayLanguage;
  const projectTitle = document.getElementById("windowProjectTitle");
  if (projectTitle) projectTitle.textContent = projectDisplayName();
  if (editorStatus) editorStatus.textContent = file ? `${displayLanguage}${isDirty(file) ? " - unsaved" : ""}` : "Start";
}

function renderDocumentTabs() {
  const tabs = document.getElementById("documentTabs");
  if (!tabs) return;
  tabs.replaceChildren();
  if (ideState.welcomeTabOpen) {
    const welcomeTab = document.createElement("button");
    welcomeTab.className = `document-tab document-tab-welcome${isWelcomeActive() ? " is-active" : ""}`;
    welcomeTab.type = "button";
    welcomeTab.dataset.tabFile = welcomeTabId;
    welcomeTab.title = "Welcome";
    welcomeTab.innerHTML = `
      ${iconSvg(Globe, "ui-icon tab-file-icon")}
      <span class="document-tab-name">Welcome</span>
      <span class="dirty-dot" aria-hidden="true"></span>
      <span class="tab-close" data-close-tab="${welcomeTabId}" aria-label="Close tab">${iconSvg(X)}</span>
    `;
    tabs.appendChild(welcomeTab);
  }
  for (const fileId of ideState.openFileIds) {
    const file = ideState.files.find((item) => item.id === fileId);
    if (!file) continue;
    const tab = document.createElement("button");
    tab.className = `document-tab${file.id === ideState.activeFileId ? " is-active" : ""}${isDirty(file) ? " is-dirty" : ""}`;
    tab.type = "button";
    tab.dataset.tabFile = file.id;
    tab.title = file.relativePath;
    tab.innerHTML = `
      ${iconSvg(fileExtension(file.name) === ".wapi" ? FileCode : FileText, "ui-icon tab-file-icon")}
      <span class="document-tab-name"></span>
      <span class="dirty-dot" aria-hidden="true"></span>
      <span class="tab-close" data-close-tab="${file.id}" aria-label="Close tab">${iconSvg(X)}</span>
    `;
    tab.querySelector(".document-tab-name").textContent = file.name;
    tabs.appendChild(tab);
  }
}

function renderProjectTreeNode(parent, node, depth = 0) {
  const folders = [...node.folders.values()].sort((a, b) => a.name.localeCompare(b.name));
  for (const folder of folders) {
    const collapsed = ideState.collapsedFolders.has(folder.path);
    const folderDirty = ideState.files.some((file) => file.relativePath.startsWith(`${folder.path}/`) && isDirty(file));
    const button = document.createElement("button");
    button.className = `tree-row tree-folder${collapsed ? " is-collapsed" : ""}${folderDirty ? " is-dirty" : ""}`;
    button.type = "button";
    button.dataset.folderPath = folder.path;
    button.style.setProperty("--depth", depth);
    button.innerHTML = `
      ${iconSvg(ChevronRight, "ui-icon chevron")}
      ${iconSvg(collapsed ? Folder : FolderOpen, "ui-icon folder-icon")}
      <span class="tree-label"></span>
      <span class="dirty-dot" aria-hidden="true"></span>
    `;
    button.querySelector(".tree-label").textContent = folder.name;
    parent.appendChild(button);
    if (!collapsed) renderProjectTreeNode(parent, folder, depth + 1);
  }

  const files = [...node.files].sort((a, b) => a.name.localeCompare(b.name));
  for (const file of files) {
    const row = document.createElement("button");
    row.className = `tree-row tree-file${file.id === ideState.activeFileId ? " is-active" : ""}${isDirty(file) ? " is-dirty" : ""}`;
    row.type = "button";
    row.dataset.fileId = file.id;
    row.style.setProperty("--depth", depth);
    row.title = file.relativePath;
    row.innerHTML = `
      <span class="tree-spacer" aria-hidden="true"></span>
      ${iconSvg(fileExtension(file.name) === ".wapi" ? FileCode : FileText, "ui-icon file-icon")}
      <span class="tree-label"></span>
      <span class="dirty-dot" aria-hidden="true"></span>
    `;
    row.querySelector(".tree-label").textContent = file.name;
    parent.appendChild(row);
  }
}

function renderSidePanel() {
  const title = document.getElementById("sideTitle");
  const meta = document.getElementById("sideMeta");
  const searchbar = document.getElementById("sideSearchbar");
  const searchInput = document.getElementById("sideSearch");
  const content = document.getElementById("sideContent");
  const actions = document.getElementById("sideActions");
  if (!content) return;

  const panelLabels = {
    explorer: "SOLUTION EXPLORER",
    search: "SEARCH",
    functions: "FUNCTIONS",
    outline: "OUTLINE",
    settings: "PROJECT"
  };
  if (title) title.textContent = panelLabels[ideState.activePanel] ?? "SOLUTION EXPLORER";
  if (searchInput && searchInput.value !== ideState.searchQuery) searchInput.value = ideState.searchQuery;
  if (searchInput) searchInput.placeholder = ideState.activePanel === "search" ? "Search project contents" : "Filter files";
  searchbar?.classList.toggle("is-hidden", !["explorer", "search"].includes(ideState.activePanel));
  actions?.classList.toggle("is-hidden", ideState.activePanel !== "explorer");
  document.getElementById("sideActionMenu")?.classList.toggle("is-open", ideState.menuOpen);

  document.querySelectorAll(".activity-button[data-panel]").forEach((button) => {
    button.classList.toggle("is-active", button.dataset.panel === ideState.activePanel);
  });

  content.replaceChildren();

  if (ideState.activePanel === "explorer") {
    const files = filteredTreeFiles();
    if (meta) meta.textContent = files.length ? `${files.length} file${files.length === 1 ? "" : "s"}` : "No project loaded";
    if (files.length === 0) {
      renderPanelEmpty(content, "START", "Create or upload a Wapi project.");
      return;
    }
    renderProjectTreeNode(content, buildProjectTree(files));
    return;
  }

  if (ideState.activePanel === "search") {
    const results = searchResults();
    if (meta) meta.textContent = ideState.searchQuery ? `${results.length} result${results.length === 1 ? "" : "s"}` : "Project search";
    if (!ideState.searchQuery.trim()) {
      renderPanelEmpty(content, "QUERY", "Type to search across project files.");
      return;
    }
    if (results.length === 0) {
      renderPanelEmpty(content, "RESULTS", "No matches found.");
      return;
    }
    for (const result of results) {
      const button = document.createElement("button");
      button.className = "search-result";
      button.type = "button";
      button.dataset.searchFile = result.file.id;
      button.dataset.searchLine = String(result.line);
      button.innerHTML = `
        <strong></strong>
        <span class="search-line"></span>
        <code></code>
      `;
      button.querySelector("strong").textContent = result.file.relativePath;
      button.querySelector(".search-line").textContent = `Line ${result.line}, Col ${result.column}`;
      button.querySelector("code").textContent = result.text;
      content.appendChild(button);
    }
    return;
  }

  if (ideState.activePanel === "functions") {
    if (meta) meta.textContent = `${wapiRuntimeFunctions.length} runtime functions`;
    const grouped = Map.groupBy
      ? Map.groupBy(wapiRuntimeFunctions, (fn) => fn.capability)
      : wapiRuntimeFunctions.reduce((map, fn) => map.set(fn.capability, [...(map.get(fn.capability) ?? []), fn]), new Map());
    for (const [capability, functions] of grouped) {
      const section = document.createElement("section");
      section.className = "side-section";
      const heading = document.createElement("div");
      heading.className = "side-section-title";
      heading.textContent = capability;
      section.appendChild(heading);
      for (const fn of functions) {
        const button = document.createElement("button");
        button.className = "function-row";
        button.type = "button";
        button.dataset.insertFunction = fn.name;
        button.innerHTML = `
          <strong></strong>
          <span></span>
        `;
        button.querySelector("strong").textContent = fn.name;
        button.querySelector("span").textContent = wapiFunctionSignature(fn);
        section.appendChild(button);
      }
      content.appendChild(section);
    }
    return;
  }

  if (ideState.activePanel === "outline") {
    const entries = outlineEntries();
    if (meta) meta.textContent = activeFile() ? activeFile().name : "No active file";
    if (!activeFile()) {
      renderPanelEmpty(content, "OUTLINE", "Open a file to see symbols.");
      return;
    }
    if (entries.length === 0) {
      renderPanelEmpty(content, "OUTLINE", "No symbols found.");
      return;
    }
    for (const entry of entries) {
      const button = document.createElement("button");
      button.className = `outline-row outline-${entry.type}`;
      button.type = "button";
      button.dataset.gotoLine = String(entry.line);
      button.innerHTML = `
        <span class="outline-kind"></span>
        <strong></strong>
        <span></span>
      `;
      button.querySelector(".outline-kind").textContent = entry.type === "function" ? "fn" : "var";
      button.querySelector("strong").textContent = entry.label;
      button.querySelector("span:last-child").textContent = `Line ${entry.line}`;
      content.appendChild(button);
    }
    return;
  }

  renderProjectSettings(content);
  if (meta) meta.textContent = ideState.projectRoot ? "Project settings" : "Runtime defaults";
}

function renderPanelEmpty(parent, label, message) {
  const wrap = document.createElement("div");
  wrap.className = "panel-empty";
  const title = document.createElement("strong");
  title.textContent = label;
  const text = document.createElement("span");
  text.textContent = message;
  wrap.append(title, text);
  parent.appendChild(wrap);
}

function renderProjectSettings(parent) {
  const config = ideState.projectConfig;
  const section = document.createElement("section");
  section.className = "settings-section";
  section.innerHTML = `
    <label class="settings-field">
      <span>Mode</span>
      <select id="settingsMode">
        <option value="safe">Safe</option>
        <option value="dev">Dev</option>
        <option value="unsafe">Unsafe</option>
      </select>
    </label>
    <label class="settings-toggle">
      <input id="settingsStrict" type="checkbox">
      <span>Strict permissions</span>
    </label>
    <label class="settings-toggle">
      <input id="settingsInjection" type="checkbox">
      <span>Allow injection</span>
    </label>
    <label class="settings-field">
      <span>Capabilities</span>
      <textarea id="settingsCapabilities" spellcheck="false"></textarea>
    </label>
    <div class="capability-cloud"></div>
  `;
  parent.appendChild(section);
  section.querySelector("#settingsMode").value = config.defaultMode;
  section.querySelector("#settingsStrict").checked = config.strictPermissions;
  section.querySelector("#settingsInjection").checked = config.allowInjection;
  section.querySelector("#settingsCapabilities").value = config.capabilities.join(", ");
  const cloud = section.querySelector(".capability-cloud");
  for (const capability of runtimeCapabilities) {
    const button = document.createElement("button");
    button.className = `capability-chip${config.capabilities.includes(capability) ? " is-active" : ""}`;
    button.type = "button";
    button.dataset.toggleCapability = capability;
    button.textContent = capability;
    cloud.appendChild(button);
  }
}

function renderToolWindow() {
  const tabs = document.getElementById("toolTabs");
  const body = document.getElementById("toolBody");
  if (!tabs || !body) return;
  body.closest(".tool-window")?.classList.toggle("is-forced-open", ideState.activeTool === "terminal");
  tabs.querySelectorAll("[data-tool]").forEach((tab) => {
    tab.classList.toggle("is-active", tab.dataset.tool === ideState.activeTool);
  });
  body.replaceChildren();

  if (ideState.activeTool === "problems") {
    if (ideState.problems.length === 0) {
      renderToolEmpty(body, "No problems found");
      return;
    }
    for (const problem of ideState.problems) {
      const button = document.createElement("button");
      button.className = "problem-row";
      button.type = "button";
      button.dataset.problemFile = problem.fileId;
      button.dataset.problemLine = String(problem.line);
      button.innerHTML = `
        <span class="problem-severity">error</span>
        <strong></strong>
        <span></span>
      `;
      button.querySelector("strong").textContent = problem.message;
      button.querySelector("span:last-child").textContent = `${problem.relativePath}:${problem.wholeFile ? "file" : problem.line}`;
      body.appendChild(button);
    }
    return;
  }

  if (ideState.activeTool === "audit") {
    renderLogLines(body, ideState.auditLines, "No audit lines yet");
    return;
  }

  if (ideState.activeTool === "terminal") {
    const wrap = document.createElement("div");
    wrap.className = "terminal-tool";
    const activeTerminal = ideState.terminalTabs.find((session) => session.id === ideState.activeTerminalId);
    const tabs = ideState.terminalTabs.map((session) => `
      <div class="terminal-session-tab${session.id === ideState.activeTerminalId ? " is-active" : ""}${session.running ? " is-running" : ""}">
        <button type="button" class="terminal-session-select" data-terminal-id="${session.id}" role="tab" aria-selected="${session.id === ideState.activeTerminalId}">
          <span class="terminal-status-dot" aria-hidden="true"></span>
          <span>${session.shell === "cmd" ? "Command Prompt" : "PowerShell"}</span>
        </button>
        <button type="button" class="terminal-session-close" data-terminal-close="${session.id}" aria-label="Close ${session.shell === "cmd" ? "Command Prompt" : "PowerShell"} terminal">${iconSvg(X)}</button>
      </div>
    `).join("");
    wrap.innerHTML = `
      <div class="terminal-session-bar">
        <div class="terminal-session-tabs" role="tablist" aria-label="Terminal sessions">${tabs}</div>
        <div class="terminal-new-controls">
          <select id="terminalShellChoice" aria-label="New terminal shell">
            <option value="powershell">PowerShell</option>
            <option value="cmd">Command Prompt</option>
          </select>
          <button id="terminalNew" class="terminal-new-button" type="button" aria-label="Create terminal">${iconSvg(Plus)}<span>New</span></button>
        </div>
      </div>
      ${activeTerminal ? '<div class="terminal-host-slot"></div>' : `
        <div class="terminal-empty">
          ${iconSvg(Terminal, "terminal-empty-icon")}
          <strong>No terminal sessions</strong>
          <span>Choose PowerShell or Command Prompt, then create a terminal.</span>
        </div>
      `}
    `;
    body.appendChild(wrap);
    if (activeTerminal) mountTerminal(activeTerminal, wrap.querySelector(".terminal-host-slot"));
    return;
  }

  renderLogLines(body, ideState.outputLines, "No output yet");
}

function renderToolEmpty(parent, message) {
  const empty = document.createElement("div");
  empty.className = "tool-empty";
  empty.textContent = message;
  parent.appendChild(empty);
}

function renderLogLines(parent, lines, emptyMessage) {
  if (!lines.length) {
    renderToolEmpty(parent, emptyMessage);
    return;
  }
  for (const line of lines) {
    const row = document.createElement("div");
    row.className = `log-line log-${line.kind}`;
    const time = document.createElement("span");
    time.className = "log-time";
    time.textContent = line.at;
    const text = document.createElement("span");
    text.textContent = line.text;
    row.append(time, text);
    parent.appendChild(row);
  }
  parent.scrollTop = parent.scrollHeight;
}

function renderStartSurface() {
  const surface = document.getElementById("startSurface");
  const monacoHost = document.getElementById("monacoEditor");
  const workbench = document.querySelector(".editor-workbench");
  const emptyHost = document.getElementById("emptyEditorState");
  const showStart = isWelcomeActive();
  const showEmpty = !showStart && !activeFile();
  surface?.classList.toggle("is-visible", showStart);
  monacoHost?.classList.toggle("is-start-hidden", showStart || showEmpty);
  workbench?.classList.toggle("is-start-mode", showStart);
  emptyHost?.classList.toggle("is-visible", showEmpty);

  const recent = document.getElementById("recentProjects");
  if (!recent) return;
  recent.replaceChildren();
  if (ideState.recentProjects.length === 0) {
    const empty = document.createElement("div");
    empty.className = "recent-empty";
    empty.textContent = "No recent projects yet";
    recent.appendChild(empty);
    return;
  }
  for (const project of ideState.recentProjects.slice(0, 5)) {
    const button = document.createElement("button");
    button.className = "recent-project";
    button.type = "button";
    button.dataset.recentRoot = project.rootPath;
    button.innerHTML = "<strong></strong><span></span>";
    button.querySelector("strong").textContent = project.name || fileName(project.rootPath);
    button.querySelector("span").textContent = project.rootPath;
    recent.appendChild(button);
  }
}

function renderToolbarState() {
  const mode = document.getElementById("runtimeMode");
  const strict = document.getElementById("runtimeStrict");
  const injection = document.getElementById("runtimeInjection");
  const caps = document.getElementById("runtimeCapabilities");
  if (mode) mode.value = ideState.projectConfig.defaultMode;
  if (strict) strict.checked = ideState.projectConfig.strictPermissions;
  if (injection) injection.checked = ideState.projectConfig.allowInjection;
  if (caps) caps.value = ideState.projectConfig.capabilities.join(", ");

  const statusMode = document.getElementById("statusRuntimeMode");
  const statusStrict = document.getElementById("statusStrict");
  const statusInjection = document.getElementById("statusInjection");
  const statusCaps = document.getElementById("statusCaps");
  if (statusMode) statusMode.textContent = ideState.projectConfig.defaultMode;
  if (statusStrict) statusStrict.textContent = ideState.projectConfig.strictPermissions ? "On" : "Off";
  if (statusInjection) statusInjection.textContent = ideState.projectConfig.allowInjection ? "Allow" : "Block";
  if (statusCaps) statusCaps.textContent = String(ideState.projectConfig.capabilities.length);
  renderRuntimeInspector();
}

function renderAll() {
  renderDocumentTabs();
  renderSidePanel();
  renderToolWindow();
  renderStartSurface();
  renderToolbarState();
  renderRuntimeInspector();
  updateStatusLine();
}

function setActivePanel(panel) {
  ideState.activePanel = panel;
  ideState.menuOpen = false;
  renderSidePanel();
}

function setActiveTool(tool) {
  document.body.classList.remove("tool-collapsed");
  ideState.activeTool = tool;
  if (tool === "terminal" && ideState.terminalTabs.length === 0) {
    createTerminalSession("powershell");
    return;
  }
  renderToolWindow();
}

function setProjectDialogOpen(open) {
  ideState.projectDialogOpen = open;
  const dialog = document.getElementById("newProjectDialog");
  const input = document.getElementById("newProjectName");
  dialog?.classList.toggle("is-open", open);
  dialog?.setAttribute("aria-hidden", String(!open));
  if (open && input) {
    input.value = "WapiProject";
    ideState.selectedTemplate = "empty";
    renderTemplatePicker();
    input.focus();
    input.select();
  }
}

function renderTemplatePicker() {
  const picker = document.getElementById("templatePicker");
  if (!picker) return;
  picker.replaceChildren();
  for (const template of projectTemplates) {
    const button = document.createElement("button");
    button.className = `template-card${template.id === ideState.selectedTemplate ? " is-active" : ""}`;
    button.type = "button";
    button.dataset.templateId = template.id;
    button.innerHTML = "<strong></strong><span></span>";
    button.querySelector("strong").textContent = template.name;
    button.querySelector("span").textContent = template.note;
    picker.appendChild(button);
  }
}

async function openCreateProjectDialog() {
  if (!confirmDiscardUnsaved()) return;
  setProjectDialogOpen(true);
}

async function createProjectFromDialog() {
  const input = document.getElementById("newProjectName");
  const name = input?.value.trim() || "WapiProject";
  const config = templateConfig(name, ideState.selectedTemplate);
  const project = await bridge.createProject({
    name,
    config,
    files: starterProjectFiles(name, ideState.selectedTemplate)
  });
  if (!project || !Array.isArray(project.files) || project.files.length === 0) {
    setProjectDialogOpen(false);
    setStatus("Project creation cancelled");
    return;
  }
  setProjectDialogOpen(false);
  await replaceProject(project, "Project created");
}

async function uploadProjectIntoExplorer(rootPath = null) {
  if (!confirmDiscardUnsaved()) return;
  const project = await bridge.loadProject(rootPath);
  if (!project || !Array.isArray(project.files)) {
    setStatus(rootPath ? "Recent project unavailable" : "Project upload cancelled");
    return;
  }
  await replaceProject(project, rootPath ? "Recent project opened" : "Project uploaded");
}

async function addFilesToExplorer() {
  const files = await bridge.addFiles();
  if (!Array.isArray(files) || files.length === 0) {
    setStatus("No files added");
    return;
  }
  const normalized = visibleProjectFiles(files);
  ideState.files = mergeProjectFiles(ideState.files, normalized);
  if (!ideState.activeFileId && normalized[0]) openFileInEditor(normalized[0].id, false);
  renderAll();
  setEditorModel(activeFile());
  setStatus(`${normalized.length} file${normalized.length === 1 ? "" : "s"} added`);
}

async function saveFile(file, forceSaveAs = false) {
  if (!file) return null;
  const result = await bridge.saveFile({
    filePath: forceSaveAs ? null : file.filePath,
    source: file.source
  });
  if (!result?.filePath) {
    setStatus("Save cancelled");
    return null;
  }
  file.filePath = result.filePath;
  file.relativePath = relativePathForFilePath(result.filePath);
  file.name = fileName(file.relativePath);
  file.originalSource = file.source;
  file.dirty = false;
  syncDirtyState();
  renderAll();
  setStatus(forceSaveAs ? "Saved as" : "Saved");
  return file;
}

async function saveActiveFile(forceSaveAs = false) {
  await saveFile(activeFile(), forceSaveAs);
}

async function saveAllFiles() {
  const dirtyFiles = ideState.files.filter(isDirty);
  if (dirtyFiles.length === 0) {
    setStatus("All files saved");
    return;
  }
  for (const file of dirtyFiles.filter((item) => !item.filePath)) {
    await saveFile(file, true);
  }
  const filesWithPaths = dirtyFiles.filter((file) => file.filePath);
  const results = await bridge.saveFiles(filesWithPaths.map((file) => ({ filePath: file.filePath, source: file.source })));
  for (const result of results) {
    if (!result.ok) continue;
    const file = ideState.files.find((item) => item.filePath === result.filePath);
    if (file) {
      file.originalSource = file.source;
      file.dirty = false;
    }
  }
  syncDirtyState();
  renderAll();
  setStatus("Save all complete");
}

function goToLine(line) {
  if (!editorState.editor) return;
  editorState.editor.revealLineInCenter(line);
  editorState.editor.setPosition({ lineNumber: line, column: 1 });
  editorState.editor.focus();
}

function insertFunctionSnippet(functionName) {
  const fn = wapiRuntimeFunctionMap.get(functionName);
  if (!fn || !editorState.editor) return;
  editorState.editor.focus();
  editorState.editor.trigger("keyboard", "editor.action.insertSnippet", { snippet: wapiFunctionSnippet(fn) });
}

function updateRuntimeFromControls(source = "toolbar") {
  const prefix = source === "settings" ? "settings" : "runtime";
  const mode = document.getElementById(`${prefix}Mode`)?.value ?? ideState.projectConfig.defaultMode;
  const strict = document.getElementById(`${prefix}Strict`)?.checked ?? ideState.projectConfig.strictPermissions;
  const injection = document.getElementById(`${prefix}Injection`)?.checked ?? ideState.projectConfig.allowInjection;
  const capsValue = document.getElementById(`${prefix}Capabilities`)?.value ?? ideState.projectConfig.capabilities.join(", ");
  ideState.projectConfig = normalizeProjectConfig({
    ...ideState.projectConfig,
    defaultMode: mode,
    strictPermissions: strict,
    allowInjection: injection,
    capabilities: capsValue.split(",").map((cap) => cap.trim()).filter(Boolean)
  }, projectDisplayName());
  renderToolbarState();
  if (source === "toolbar") renderSidePanel();
  persistProjectConfig();
  setStatus("Runtime settings updated");
}

function terminalSession(id) {
  return ideState.terminalTabs.find((session) => session.id === id);
}

function updateTerminalTabState(session) {
  const tab = document.querySelector(`.terminal-session-tab:has([data-terminal-id="${session.id}"])`);
  tab?.classList.toggle("is-running", session.running);
}

async function startTerminalSession(session, runtime) {
  if (runtime.starting || session.running) return;
  runtime.starting = true;
  const result = await bridge.terminal.start({
    sessionId: session.id,
    cwd: ideState.projectRoot || undefined,
    shell: session.shell,
    cols: runtime.terminal.cols,
    rows: runtime.terminal.rows
  });
  runtime.starting = false;
  session.running = Boolean(result?.ok);
  session.cwd = result?.cwd || "";
  updateTerminalTabState(session);
  if (!result?.ok) runtime.terminal.writeln(`\x1b[31mUnable to start terminal: ${result?.stderr || "Unknown error"}\x1b[0m`);
  runtime.terminal.focus();
}

function createTerminalRuntime(session, slot) {
  const terminal = new XtermTerminal({
    cursorBlink: true,
    cursorStyle: "bar",
    fontFamily: '"Cascadia Mono", "Cascadia Code", Consolas, monospace',
    fontSize: 13,
    lineHeight: 1.15,
    scrollback: 5000,
    theme: {
      background: "#090a09",
      foreground: "#c4c7c4",
      cursor: "#4ade80",
      cursorAccent: "#090a09",
      selectionBackground: "#224d2e",
      black: "#090a09",
      brightBlack: "#575b57",
      green: "#4ade80",
      brightGreen: "#86efac"
    }
  });
  const fitAddon = new FitAddon();
  const host = document.createElement("div");
  host.className = "terminal-host";
  slot.appendChild(host);
  terminal.loadAddon(fitAddon);
  terminal.open(host);

  const runtime = { terminal, fitAddon, host, starting: false, resizeObserver: null };
  terminal.onData((data) => bridge.terminal.send({ sessionId: session.id, data }));
  terminal.onResize(({ cols, rows }) => bridge.terminal.resize({ sessionId: session.id, cols, rows }));
  terminalRuntimes.set(session.id, runtime);
  startTerminalSession(session, runtime);
  return runtime;
}

function mountTerminal(session, slot) {
  const runtime = terminalRuntimes.get(session.id) || createTerminalRuntime(session, slot);
  if (runtime.host.parentElement !== slot) slot.appendChild(runtime.host);
  runtime.resizeObserver?.disconnect();
  runtime.resizeObserver = new ResizeObserver(() => {
    if (runtime.host.isConnected) runtime.fitAddon.fit();
  });
  runtime.resizeObserver.observe(slot);
  requestAnimationFrame(() => {
    runtime.fitAddon.fit();
    runtime.terminal.focus();
  });
}

function createTerminalSession(shell = "powershell") {
  const session = {
    id: `terminal-${++terminalSequence}`,
    shell: shell === "cmd" ? "cmd" : "powershell",
    running: false,
    cwd: ""
  };
  ideState.terminalTabs.push(session);
  ideState.activeTerminalId = session.id;
  ideState.activeTool = "terminal";
  renderToolWindow();
}

async function closeTerminalSession(sessionId) {
  const index = ideState.terminalTabs.findIndex((session) => session.id === sessionId);
  if (index < 0) return;
  const runtime = terminalRuntimes.get(sessionId);
  runtime?.resizeObserver?.disconnect();
  runtime?.terminal.dispose();
  terminalRuntimes.delete(sessionId);
  await bridge.terminal.stop({ sessionId });
  ideState.terminalTabs.splice(index, 1);
  if (ideState.activeTerminalId === sessionId) {
    ideState.activeTerminalId = ideState.terminalTabs[Math.min(index, ideState.terminalTabs.length - 1)]?.id || null;
  }
  renderToolWindow();
}

function installTerminalListener() {
  bridge.terminal.onData((payload = {}) => {
    const session = terminalSession(payload.sessionId);
    const runtime = terminalRuntimes.get(payload.sessionId);
    if (!session || !runtime) return;
    if (payload.type === "data") runtime.terminal.write(payload.data || "");
    if (payload.type === "exit") {
      session.running = false;
      runtime.terminal.writeln(`\r\n\x1b[90mProcess exited with code ${payload.exitCode ?? "unknown"}.\x1b[0m`);
      updateTerminalTabState(session);
    }
  });
}

async function requestWindowClose() {
  if (!confirmDiscardUnsaved()) return;
  await bridge.window.setDirtyState(false);
  await bridge.window.close();
}

function installRendererIcon() {
  const existing = document.querySelector("link[rel='icon']");
  const link = existing || document.createElement("link");
  link.rel = "icon";
  link.type = "image/png";
  link.href = wapiIconUrl;
  if (!existing) document.head.appendChild(link);
}

function renderWindowBar() {
  const root = document.getElementById("root");
  root.innerHTML = `
    <main class="app-shell">
      <header id="windowChrome" data-tauri-drag-region>
        <div class="window-brand" data-tauri-drag-region>
          <span class="window-wordmark">WAPI</span>
          <span class="window-brand-rule" aria-hidden="true"></span>
          <span id="windowProjectTitle" class="window-project-title">WapiProject</span>
          <span class="window-branch"><span aria-hidden="true">branch</span> main</span>
        </div>
        <div id="windowButtons">
          <button id="windowMinimize" class="window-btn" type="button" aria-label="Minimize">${iconSvg(Minus)}</button>
          <button id="windowMaximize" class="window-btn" type="button" aria-label="Maximize">${iconSvg(Maximize2)}</button>
          <button id="windowClose" class="window-btn window-btn-close" type="button" aria-label="Close">${iconSvg(X)}</button>
        </div>
      </header>

      <section class="workspace" aria-label="Wapi IDE workspace">
        <nav class="activity-bar" aria-label="Primary navigation">
          <button class="activity-button is-active" type="button" title="Explorer" data-panel="explorer">
            ${iconSvg(FolderTree)}<span>Explorer</span>
          </button>
          <button class="activity-button" type="button" title="Search" data-panel="search">
            ${iconSvg(Search)}<span>Search</span>
          </button>
          <button class="activity-button" type="button" title="Runtime knowledge" data-panel="functions">
            ${iconSvg(Database)}<span>Knowledge</span>
          </button>
          <button class="activity-button" type="button" title="Outline" data-panel="outline">
            ${iconSvg(ListTree)}<span>Outline</span>
          </button>
          <div class="activity-spacer" aria-hidden="true"></div>
          <button class="activity-button" type="button" title="Project settings" data-panel="settings">
            ${iconSvg(Settings)}<span>Settings</span>
          </button>
        </nav>

        <aside class="side-panel" aria-label="Side panel">
          <div class="side-topbar">
            <div>
              <div id="sideTitle" class="side-title">EXPLORER</div>
              <div id="sideMeta" class="side-meta">No project loaded</div>
            </div>
            <div id="sideActions" class="side-actions">
              <button id="sideCreateProject" class="icon-button" type="button" title="Create project">${iconSvg(Plus)}</button>
              <button id="sideUploadProject" class="icon-button" type="button" title="Load project">${iconSvg(Upload)}</button>
              <button id="sideMoreActions" class="icon-button" type="button" title="More actions">${iconSvg(Ellipsis)}</button>
            </div>
            <div id="sideActionMenu" class="side-action-menu" role="menu">
              <button class="menu-item" type="button" data-menu-action="create-project">Create new project</button>
              <button class="menu-item" type="button" data-menu-action="upload-project">Load project</button>
              <button class="menu-item" type="button" data-menu-action="add-files">Add files</button>
              <button class="menu-item" type="button" data-menu-action="save-all">Save all</button>
              <button class="menu-item" type="button" data-menu-action="clear-search">Clear search</button>
            </div>
          </div>
          <div id="sideSearchbar" class="side-searchbar">
            <div class="search-wrap">
              ${iconSvg(Search)}
              <input id="sideSearch" type="search" spellcheck="false" autocomplete="off" placeholder="Filter files">
            </div>
          </div>
          <div id="sideContent" class="side-content"></div>
          <div class="side-footer">
            <button type="button" data-panel-jump="outline">${iconSvg(ListTree)}<span>Outline</span></button>
            <button type="button" data-panel-jump="functions">${iconSvg(Database)}<span>Dependencies</span></button>
          </div>
        </aside>

        <section class="editor-surface" aria-label="Editor">
          <div class="menu-strip">
            <div class="runtime-controls" aria-label="Runtime controls">
              <label class="runtime-field runtime-mode">
                <span>Runtime</span>
                <select id="runtimeMode" title="Runtime mode">
                  <option value="safe">Safe</option>
                  <option value="dev">Dev</option>
                  <option value="unsafe">Unsafe</option>
                </select>
              </label>
              <label class="runtime-field runtime-switch">
                <span>Strict</span>
                <span class="switch-control"><input id="runtimeStrict" type="checkbox"><b>On</b></span>
              </label>
              <label class="runtime-field runtime-switch">
                <span>Injection</span>
                <span class="switch-control"><input id="runtimeInjection" type="checkbox"><b>Block</b></span>
              </label>
              <label class="runtime-field runtime-caps">
                <span>Capabilities</span>
                <span class="capability-input">${iconSvg(Database)}<input id="runtimeCapabilities" type="text" spellcheck="false" title="Capabilities" placeholder="proc.list"></span>
              </label>
            </div>

            <div class="toolbar-actions">
              <div class="save-actions">
                <button id="toolbarSave" class="toolbar-icon" type="button" title="Save">${iconSvg(Save)}</button>
                <button id="toolbarSaveAs" class="toolbar-icon" type="button" title="Save as">${iconSvg(FileText)}</button>
                <button id="toolbarSaveAll" class="toolbar-icon" type="button" title="Save all">${iconSvg(SaveAll)}</button>
              </div>
              <button id="toolbarInspector" class="toolbar-inspector" type="button" aria-label="Runtime Inspector" aria-expanded="false">
                ${iconSvg(Activity)}<span>Runtime Inspector</span>${iconSvg(ChevronDown, "ui-icon disclosure")}
              </button>
              <button id="toolbarCheck" class="toolbar-button toolbar-button-primary" type="button">
                ${iconSvg(ShieldCheck)}<span>Check</span>
              </button>
              <button id="toolbarRun" class="toolbar-button toolbar-button-run" type="button">
                ${iconSvg(Play)}<span>Run</span>
              </button>
            </div>
          </div>

          <aside id="runtimeInspectorDrawer" class="runtime-inspector-drawer" aria-hidden="true">
            <div class="inspector-topline">
              <div><span class="inspector-kicker">Live runtime</span><strong>Runtime Inspector</strong></div>
              <button id="runtimeInspectorClose" class="icon-button" type="button" aria-label="Close runtime inspector">${iconSvg(X)}</button>
            </div>
            <div class="inspector-grid">
              <section class="inspector-section">
                <h2>${iconSvg(Cpu)} Process</h2>
                <div data-inspector-process></div>
              </section>
              <section class="inspector-section">
                <h2>${iconSvg(ShieldCheck)} Capabilities</h2>
                <div data-inspector-capabilities></div>
              </section>
              <section class="inspector-section">
                <h2>${iconSvg(Database)} Memory Map</h2>
                <div data-inspector-memory></div>
              </section>
              <section class="inspector-section inspector-events">
                <h2>${iconSvg(Activity)} Live Events</h2>
                <div data-inspector-events></div>
              </section>
            </div>
          </aside>

          <div id="documentTabs" class="document-tabs"></div>
          <div class="editor-workbench">
            <section id="startSurface" class="start-surface" aria-label="Start">
              <div class="start-shell">
                <div class="start-identity">
                  <div class="start-mark" aria-hidden="true">WAPI</div>
                  <h1 class="start-title">Precision access to Windows internals.</h1>
                  <p class="start-subtitle">Create or load a Wapi project. Check is the primary action; runtime permissions stay explicit.</p>
                  <div class="recent-title">Recent Projects</div>
                  <div id="recentProjects" class="recent-list"></div>
                </div>
                <div class="start-actions-panel">
                  <button id="startCreateProject" class="start-action start-action-primary" type="button">
                    <span class="start-action-index">01</span>
                    <span><span class="start-action-title">Create project</span><span class="start-action-note">Empty, Process Inspector, or Memory Sandbox</span></span>
                    ${iconSvg(ArrowRight, "ui-icon start-action-arrow")}
                  </button>
                  <button id="startUploadProject" class="start-action" type="button">
                    <span class="start-action-index">02</span>
                    <span><span class="start-action-title">Load project</span><span class="start-action-note">Open an existing Wapi workspace</span></span>
                    ${iconSvg(ArrowRight, "ui-icon start-action-arrow")}
                  </button>
                </div>
              </div>
            </section>
            <div id="emptyEditorState" class="empty-editor-state" aria-hidden="true">
              <img src="${wapiIconUrl}" alt="">
            </div>
            <div id="monacoEditor"></div>
          </div>

          <section class="tool-window" aria-label="Tool windows">
            <div id="toolTabs" class="tool-tabs">
              <button class="tool-tab" type="button" data-tool="problems">Problems</button>
              <button class="tool-tab is-active" type="button" data-tool="output">Output</button>
              <button class="tool-tab" type="button" data-tool="terminal">Terminal</button>
              <button class="tool-tab" type="button" data-tool="audit">Audit</button>
              <span class="tool-tabs-spacer"></span>
              <button class="tool-utility" type="button" title="New terminal" data-new-terminal>${iconSvg(Plus)}</button>
              <button class="tool-utility" type="button" title="Close panel" data-close-tool>${iconSvg(X)}</button>
            </div>
            <div id="toolBody" class="tool-body"></div>
          </section>
        </section>
      </section>

      <footer class="status-bar">
        <div class="status-left">
          <span class="status-mode">${iconSvg(ShieldCheck)}<b id="statusRuntimeMode">Safe</b></span>
          <span>Strict: <b id="statusStrict">On</b></span>
          <span>Injection: <b id="statusInjection">Block</b></span>
          <span>Caps: <b id="statusCaps">0</b></span>
          <span id="statusFile" class="status-file">Welcome</span>
          <span id="statusDirty">Saved</span>
        </div>
        <div class="status-right">
          <span id="statusCursor">Ln 1, Col 1</span>
          <span>Spaces: 4</span>
          <span>UTF-8</span>
          <span id="statusLanguage">Wapi</span>
          <button id="statusTerminal" class="status-button" type="button">${iconSvg(Terminal)}<span>Terminal</span></button>
          <span class="status-dot" aria-hidden="true"></span>
          <span id="statusInstance">Ready</span>
        </div>
      </footer>

      <div id="newProjectDialog" class="project-dialog" aria-hidden="true">
        <form id="newProjectForm" class="project-dialog-card">
          <div class="dialog-heading">
            <span>New workspace</span>
            <h2>Create a Wapi project</h2>
          </div>
          <label class="project-field">
            <span>Project name</span>
            <input id="newProjectName" type="text" autocomplete="off" spellcheck="false" value="WapiProject">
          </label>
          <div id="templatePicker" class="template-picker"></div>
          <div class="project-dialog-actions">
            <button id="newProjectCancel" class="dialog-button" type="button">Cancel</button>
            <button class="dialog-button dialog-button-primary" type="submit">Create project</button>
          </div>
        </form>
      </div>
    </main>
  `;
}

function bindEvents() {
  document.getElementById("windowMinimize")?.addEventListener("click", () => bridge.window.minimize());
  document.getElementById("windowMaximize")?.addEventListener("click", () => bridge.window.toggleMaximize());
  document.getElementById("windowClose")?.addEventListener("click", requestWindowClose);
  document.getElementById("windowChrome")?.addEventListener("dblclick", (event) => {
    if (!event.target.closest("button")) bridge.window.toggleMaximize();
  });
  document.getElementById("toolbarInspector")?.addEventListener("click", () => {
    ideState.inspectorOpen = !ideState.inspectorOpen;
    renderRuntimeInspector();
  });
  document.getElementById("runtimeInspectorClose")?.addEventListener("click", () => {
    ideState.inspectorOpen = false;
    renderRuntimeInspector();
  });
  document.getElementById("sideCreateProject")?.addEventListener("click", openCreateProjectDialog);
  document.getElementById("sideUploadProject")?.addEventListener("click", () => uploadProjectIntoExplorer());
  document.getElementById("sideMoreActions")?.addEventListener("click", () => {
    ideState.menuOpen = !ideState.menuOpen;
    renderSidePanel();
  });
  document.getElementById("startCreateProject")?.addEventListener("click", openCreateProjectDialog);
  document.getElementById("startUploadProject")?.addEventListener("click", () => uploadProjectIntoExplorer());
  document.getElementById("recentProjects")?.addEventListener("click", async (event) => {
    const recent = event.target.closest("[data-recent-root]");
    if (!recent) return;
    await uploadProjectIntoExplorer(recent.dataset.recentRoot);
  });
  document.getElementById("toolbarSave")?.addEventListener("click", () => saveActiveFile(false));
  document.getElementById("toolbarSaveAs")?.addEventListener("click", () => saveActiveFile(true));
  document.getElementById("toolbarSaveAll")?.addEventListener("click", saveAllFiles);
  document.getElementById("toolbarCheck")?.addEventListener("click", () => runWapiCommand("check"));
  document.getElementById("toolbarRun")?.addEventListener("click", () => runWapiCommand("run"));
  document.getElementById("statusTerminal")?.addEventListener("click", () => setActiveTool("terminal"));

  ["runtimeMode", "runtimeStrict", "runtimeInjection", "runtimeCapabilities"].forEach((id) => {
    document.getElementById(id)?.addEventListener(id === "runtimeCapabilities" ? "change" : "input", () => updateRuntimeFromControls("toolbar"));
  });

  document.querySelectorAll(".activity-button[data-panel]").forEach((button) => {
    button.addEventListener("click", () => setActivePanel(button.dataset.panel));
  });

  document.getElementById("sideSearch")?.addEventListener("input", (event) => {
    ideState.searchQuery = event.target.value;
    renderSidePanel();
  });

  document.getElementById("sideContent")?.addEventListener("click", (event) => {
    const folder = event.target.closest("[data-folder-path]");
    if (folder) {
      const path = folder.dataset.folderPath;
      if (ideState.collapsedFolders.has(path)) ideState.collapsedFolders.delete(path);
      else ideState.collapsedFolders.add(path);
      renderSidePanel();
      return;
    }

    const fileRow = event.target.closest("[data-file-id]");
    if (fileRow) {
      openFileInEditor(fileRow.dataset.fileId);
      return;
    }

    const result = event.target.closest("[data-search-file]");
    if (result) {
      openFileInEditor(result.dataset.searchFile);
      goToLine(Number(result.dataset.searchLine || "1"));
      return;
    }

    const outline = event.target.closest("[data-goto-line]");
    if (outline) {
      goToLine(Number(outline.dataset.gotoLine || "1"));
      return;
    }

    const fn = event.target.closest("[data-insert-function]");
    if (fn) {
      insertFunctionSnippet(fn.dataset.insertFunction);
      return;
    }

    const capability = event.target.closest("[data-toggle-capability]");
    if (capability) {
      const cap = capability.dataset.toggleCapability;
      const caps = new Set(ideState.projectConfig.capabilities);
      if (caps.has(cap)) caps.delete(cap);
      else caps.add(cap);
      ideState.projectConfig.capabilities = [...caps].sort();
      persistProjectConfig();
      renderSidePanel();
      renderToolbarState();
    }
  });

  document.getElementById("sideActionMenu")?.addEventListener("click", async (event) => {
    const action = event.target.closest("[data-menu-action]")?.dataset.menuAction;
    ideState.menuOpen = false;
    if (action === "create-project") await openCreateProjectDialog();
    if (action === "upload-project") await uploadProjectIntoExplorer();
    if (action === "add-files") await addFilesToExplorer();
    if (action === "save-all") await saveAllFiles();
    if (action === "clear-search") {
      ideState.searchQuery = "";
      renderSidePanel();
      setStatus("Search cleared");
    }
  });

  document.getElementById("documentTabs")?.addEventListener("click", (event) => {
    const close = event.target.closest("[data-close-tab]");
    if (close) {
      event.stopPropagation();
      closeDocumentTab(close.dataset.closeTab);
      return;
    }
    const tab = event.target.closest("[data-tab-file]");
    if (tab) {
      if (tab.dataset.tabFile === welcomeTabId) {
        ideState.activeFileId = welcomeTabId;
        renderAll();
        setEditorModel(null);
      } else {
        openFileInEditor(tab.dataset.tabFile);
      }
    }
  });

  document.getElementById("toolTabs")?.addEventListener("click", (event) => {
    const tab = event.target.closest("[data-tool]");
    if (tab) setActiveTool(tab.dataset.tool);
  });

  document.getElementById("toolBody")?.addEventListener("click", (event) => {
    const problem = event.target.closest("[data-problem-file]");
    if (problem) {
      openFileInEditor(problem.dataset.problemFile);
      goToLine(Number(problem.dataset.problemLine || "1"));
      return;
    }
    const closeTerminal = event.target.closest("[data-terminal-close]");
    if (closeTerminal) {
      closeTerminalSession(closeTerminal.dataset.terminalClose);
      return;
    }
    const selectTerminal = event.target.closest("[data-terminal-id]");
    if (selectTerminal) {
      ideState.activeTerminalId = selectTerminal.dataset.terminalId;
      renderToolWindow();
      return;
    }
    if (event.target.closest("#terminalNew")) {
      const shell = document.getElementById("terminalShellChoice")?.value || "powershell";
      createTerminalSession(shell);
    }
  });

  document.getElementById("newProjectCancel")?.addEventListener("click", () => setProjectDialogOpen(false));
  document.getElementById("newProjectDialog")?.addEventListener("click", (event) => {
    if (event.target.id === "newProjectDialog") setProjectDialogOpen(false);
  });
  document.getElementById("templatePicker")?.addEventListener("click", (event) => {
    const template = event.target.closest("[data-template-id]");
    if (!template) return;
    ideState.selectedTemplate = template.dataset.templateId;
    renderTemplatePicker();
  });
  document.getElementById("newProjectForm")?.addEventListener("submit", async (event) => {
    event.preventDefault();
    await createProjectFromDialog();
  });

  document.addEventListener("keydown", async (event) => {
    const modifier = event.ctrlKey || event.metaKey;
    if (modifier && ["+", "-", "=", "0"].includes(event.key)) {
      event.preventDefault();
      return;
    }
    if (event.key === "Escape" && ideState.inspectorOpen) {
      ideState.inspectorOpen = false;
      renderRuntimeInspector();
      return;
    }
    if (!modifier) return;
    const key = event.key.toLowerCase();
    if (key === "s" && event.shiftKey) {
      event.preventDefault();
      await saveAllFiles();
    } else if (key === "s") {
      event.preventDefault();
      await saveActiveFile(false);
    } else if (key === "f") {
      event.preventDefault();
      setActivePanel("search");
      document.getElementById("sideSearch")?.focus();
    }
  });

  window.addEventListener("wheel", (event) => {
    if (event.ctrlKey || event.metaKey) event.preventDefault();
  }, { passive: false });

  window.addEventListener("beforeunload", (event) => {
    if (!ideState.dirty) return;
    event.preventDefault();
    event.returnValue = "";
  });
}

async function init() {
  installRendererIcon();
  installMonacoLanguage();
  renderWindowBar();
  bindEvents();
  installTerminalListener();
  createMonacoEditor();
  renderTemplatePicker();
  renderAll();

  ideState.recentProjects = await bridge.listRecentProjects();
  renderStartSurface();
  syncDirtyState(false);
}

init();
