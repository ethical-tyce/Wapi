import wapiIconUrl from "../wapi.png";
import bannerIconUrl from "../banner icon.png";
import evaluatorSource from "../evaluator.cpp?raw";
import {
  ArrowRight,
  Braces,
  ChevronRight,
  Ellipsis,
  FileCode,
  FileText,
  Folder,
  FolderOpen,
  ListTree,
  Maximize2,
  Minus,
  PanelLeft,
  Play,
  Plus,
  Save,
  SaveAll,
  Search,
  Settings,
  SquareCheck,
  Terminal,
  Upload,
  X
} from "lucide";
import * as monaco from "monaco-editor/esm/vs/editor/editor.api";
import editorWorker from "monaco-editor/esm/vs/editor/editor.worker?worker";

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

const bridge = window.wapi ?? {
  execute: async (payload = {}) => ({
    ok: false,
    code: null,
    stdout: "",
    stderr: `Native Wapi ${payload.command ?? "run"} is available in the Electron app, not the browser preview.`,
    exe: null
  }),
  locate: async () => null,
  addFiles: async () => [],
  createProject: async (payload = {}) => ({
    rootPath: null,
    config: payload.config ?? null,
    files: payload.files ?? []
  }),
  loadProject: async () => null,
  openFile: async () => null,
  saveFile: async (payload = {}) => ({ filePath: payload.filePath ?? "browser-preview.wapi" }),
  saveFiles: async (files = []) => files.map((file) => ({ ok: true, filePath: file.filePath })),
  readProjectConfig: async () => null,
  writeProjectConfig: async (_rootPath, config) => config,
  listRecentProjects: async () => [],
  addRecentProject: async () => [],
  shell: async () => ({ ok: true, stdout: "", stderr: "" }),
  terminal: {
    start: async () => ({ ok: true, shell: "powershell", cwd: "" }),
    send: async () => ({ ok: true }),
    stop: async () => ({ ok: true }),
    onData: () => () => null
  },
  window: {
    minimize: async () => null,
    toggleMaximize: async () => null,
    close: async () => null,
    setDirtyState: async () => null
  }
};

const visibleExtensions = new Set([".wapi", ".txt", ".json", ".cpp", ".c", ".h", ".hpp"]);
const maxPanelLines = 300;
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
  activeFileId: null,
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
  terminalLines: [],
  problems: [],
  terminalRunning: false,
  terminalInput: ""
};

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
  return ideState.files.find((file) => file.id === ideState.activeFileId) ?? null;
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
  ideState.activeFileId = null;
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
  ideState.openFileIds = ideState.openFileIds.filter((id) => id !== fileId);
  if (ideState.activeFileId === fileId) {
    ideState.activeFileId = ideState.openFileIds.at(-1) ?? null;
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

  const result = await bridge.execute({ command, source, options: runtimeOptions() });
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
      { token: "comment", foreground: "747a86" },
      { token: "keyword", foreground: "f4f7fb", fontStyle: "bold" },
      { token: "type.identifier", foreground: "82d8af" },
      { token: "string", foreground: "c8d7ff" },
      { token: "number", foreground: "f0c78a" }
    ],
    colors: {
      "editor.background": "#18191e",
      "editor.foreground": "#ced3dc",
      "editorLineNumber.foreground": "#777d8a",
      "editorLineNumber.activeForeground": "#d8dde6",
      "editorCursor.foreground": "#dce2eb",
      "editor.selectionBackground": "#314354",
      "editor.inactiveSelectionBackground": "#292e37",
      "editor.lineHighlightBackground": "#20222a",
      "editorIndentGuide.background1": "#2d3038",
      "editorIndentGuide.activeBackground1": "#4b5260",
      "editorWidget.background": "#1d1f26",
      "editorWidget.border": "#373d49",
      "input.background": "#252832",
      "input.border": "#363c48"
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
  if (editorStatus) editorStatus.textContent = file ? `${displayLanguage}${isDirty(file) ? " - unsaved" : ""}` : "Start";
}

function renderDocumentTabs() {
  const tabs = document.getElementById("documentTabs");
  if (!tabs) return;
  tabs.replaceChildren();
  if (ideState.openFileIds.length === 0) {
    const empty = document.createElement("div");
    empty.className = "document-tab is-empty";
    empty.textContent = "Welcome";
    tabs.appendChild(empty);
    return;
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
    wrap.innerHTML = `
      <div class="terminal-log"></div>
      <form id="terminalForm" class="terminal-input-row">
        <button id="terminalStart" class="tool-action" type="button">${ideState.terminalRunning ? "Restart" : "Start"}</button>
        <input id="terminalInput" type="text" spellcheck="false" autocomplete="off" placeholder="PowerShell command">
        <button class="tool-action tool-action-primary" type="submit">Send</button>
      </form>
    `;
    body.appendChild(wrap);
    renderLogLines(wrap.querySelector(".terminal-log"), ideState.terminalLines, "Terminal is idle");
    const input = wrap.querySelector("#terminalInput");
    input.value = ideState.terminalInput;
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
  const showStart = ideState.files.length === 0;
  surface?.classList.toggle("is-visible", showStart);
  monacoHost?.classList.toggle("is-start-hidden", showStart);
  workbench?.classList.toggle("is-start-mode", showStart);

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
}

function renderAll() {
  renderDocumentTabs();
  renderSidePanel();
  renderToolWindow();
  renderStartSurface();
  renderToolbarState();
  updateStatusLine();
}

function setActivePanel(panel) {
  ideState.activePanel = panel;
  ideState.menuOpen = false;
  renderSidePanel();
}

function setActiveTool(tool) {
  ideState.activeTool = tool;
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
    setStatus("Project upload cancelled");
    return;
  }
  await replaceProject(project, "Project uploaded");
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

async function startTerminal() {
  const result = await bridge.terminal.start({ cwd: ideState.projectRoot || undefined, shell: "powershell" });
  ideState.terminalRunning = Boolean(result?.ok);
  appendLines(ideState.terminalLines, [`Terminal ${ideState.terminalRunning ? "started" : "failed"}${result?.cwd ? ` in ${result.cwd}` : ""}`], ideState.terminalRunning ? "success" : "error");
  ideState.activeTool = "terminal";
  renderToolWindow();
}

async function sendTerminalCommand(command) {
  if (!command.trim()) return;
  if (!ideState.terminalRunning) await startTerminal();
  appendLines(ideState.terminalLines, [`PS> ${command}`], "command");
  ideState.terminalInput = "";
  await bridge.terminal.send({ command });
  renderToolWindow();
}

function installTerminalListener() {
  bridge.terminal.onData((payload = {}) => {
    const kind = payload.type === "stderr" ? "error" : payload.type === "exit" ? "warning" : "info";
    if (payload.type === "exit") ideState.terminalRunning = false;
    appendLines(ideState.terminalLines, splitLines(payload.text), kind);
    if (ideState.activeTool === "terminal") renderToolWindow();
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

function installStyles() {
  const style = document.createElement("style");
  style.textContent = `
    :root {
      color-scheme: dark;
      font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text", "Segoe UI", sans-serif;
      --chrome-height: 38px;
      --status-height: 24px;
      --activity-width: 58px;
      --side-width: 286px;
      --tool-height: 190px;
      --accent: #37c685;
      --accent-blue: #73a7ff;
      --danger: #e26b6b;
      --warning: #e2b86b;
      --bg: #15161b;
      --panel: #191a20;
      --panel-2: #1f2129;
      --panel-3: #252833;
      --line: rgba(230, 235, 246, 0.075);
      --line-strong: rgba(230, 235, 246, 0.14);
      --text: #d7dce6;
      --text-soft: #aab2c2;
      --text-muted: #7f8797;
      --shadow: 0 18px 46px rgba(0, 0, 0, 0.32);
      --focus: 0 0 0 1px rgba(55, 198, 133, 0.5), 0 0 0 4px rgba(55, 198, 133, 0.12);
      --fast: 120ms cubic-bezier(.2, .7, .2, 1);
      --med: 190ms cubic-bezier(.2, .7, .2, 1);
      --slow: 280ms cubic-bezier(.16, 1, .3, 1);
    }

    * { box-sizing: border-box; }
    html, body, #root { width: 100%; height: 100%; margin: 0; overflow: hidden; }
    body {
      color: var(--text);
      background: var(--bg);
      -webkit-user-select: none;
      user-select: none;
    }
    button, input, select, textarea { font: inherit; }
    button:focus-visible, input:focus-visible, select:focus-visible, textarea:focus-visible {
      outline: none;
      box-shadow: var(--focus);
    }

    @keyframes surfaceIn {
      from { opacity: 0; transform: translateY(6px); }
      to { opacity: 1; transform: translateY(0); }
    }
    @keyframes menuIn {
      from { opacity: 0; transform: translateY(-6px) scale(.98); }
      to { opacity: 1; transform: translateY(0) scale(1); }
    }
    @keyframes livePulse {
      0% { color: #f6fff9; text-shadow: 0 0 12px rgba(55, 198, 133, .46); }
      100% { color: inherit; text-shadow: none; }
    }

    .app-shell {
      display: grid;
      grid-template-rows: var(--chrome-height) 1fr var(--status-height);
      width: 100%;
      height: 100%;
      background: linear-gradient(180deg, #18191f, #14151a);
    }

    #windowChrome {
      display: grid;
      grid-template-columns: 1fr auto 1fr;
      align-items: center;
      height: var(--chrome-height);
      border: 1px solid var(--line);
      border-bottom-color: var(--line-strong);
      background: linear-gradient(180deg, rgba(255,255,255,.025), transparent), #17181e;
      -webkit-app-region: drag;
    }
    #windowBannerFrame {
      position: relative;
      width: 82px;
      height: 22px;
      margin-left: 14px;
      overflow: hidden;
    }
    #windowBannerIcon {
      position: absolute;
      left: 0;
      top: -5px;
      height: 33px;
      width: auto;
    }
    #windowAppTitle {
      justify-self: center;
      color: var(--text-soft);
      font-size: 12px;
      font-weight: 650;
    }
    #windowButtons {
      justify-self: end;
      display: flex;
      gap: 8px;
      padding-right: 10px;
      -webkit-app-region: no-drag;
    }
    .window-btn {
      position: relative;
      width: 30px;
      height: 30px;
      border: 0;
      border-radius: 5px;
      color: #9ca4b4;
      background: transparent;
      cursor: pointer;
      transition: color var(--fast), background var(--fast), transform var(--fast);
    }
    .window-btn:hover {
      color: #eef2f8;
      background: rgba(230, 235, 246, .07);
      transform: translateY(-1px);
    }
    .window-btn:active { transform: translateY(0) scale(.95); }
    .window-btn-close:hover { background: #a63d42; }
    .window-icon, .window-icon::before, .window-icon::after {
      position: absolute;
      left: 50%;
      top: 50%;
      content: "";
      transform: translate(-50%, -50%);
    }
    .window-icon-min::before {
      width: 12px;
      height: 1.6px;
      border-radius: 2px;
      background: currentColor;
    }
    .window-icon-max::before {
      width: 11px;
      height: 11px;
      border: 1.6px solid currentColor;
      border-radius: 2px;
    }
    .window-icon-close::before, .window-icon-close::after {
      width: 13px;
      height: 1.5px;
      border-radius: 2px;
      background: currentColor;
    }
    .window-icon-close::before { transform: translate(-50%, -50%) rotate(45deg); }
    .window-icon-close::after { transform: translate(-50%, -50%) rotate(-45deg); }

    .workspace {
      display: grid;
      grid-template-columns: var(--activity-width) var(--side-width) 1fr;
      min-height: 0;
      background: var(--bg);
    }
    .activity-bar {
      display: flex;
      flex-direction: column;
      gap: 8px;
      padding: 12px 0;
      border-right: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(255,255,255,.018), rgba(0,0,0,.05)), #17181e;
    }
    .activity-spacer { flex: 1; }
    .activity-button {
      position: relative;
      display: grid;
      width: 44px;
      height: 44px;
      margin-inline: auto;
      place-items: center;
      border: 0;
      border-radius: 8px;
      color: #8f98aa;
      background: transparent;
      cursor: pointer;
      transition: color var(--fast), background var(--fast), transform var(--fast), box-shadow var(--med);
    }
    .activity-button::before {
      position: absolute;
      left: -7px;
      top: 50%;
      width: 3px;
      height: 18px;
      border-radius: 99px;
      background: var(--accent);
      content: "";
      opacity: 0;
      transform: translateY(-50%) scaleY(.45);
      transition: opacity var(--fast), transform var(--med);
    }
    .activity-button:hover {
      color: #edf2f8;
      background: rgba(230, 235, 246, .06);
      transform: translateY(-1px);
    }
    .activity-button.is-active {
      color: #101713;
      background: linear-gradient(135deg, var(--accent), var(--accent-blue));
      box-shadow: 0 10px 28px rgba(55, 198, 133, .15);
    }
    .activity-button.is-active::before {
      opacity: 1;
      transform: translateY(-50%) scaleY(1);
    }
    .activity-button:active { transform: scale(.96); }

    .ui-icon {
      display: block;
      width: 17px;
      height: 17px;
      fill: none;
      stroke: currentColor;
      stroke-width: 1.8;
      stroke-linecap: round;
      stroke-linejoin: round;
      vector-effect: non-scaling-stroke;
      transition: transform var(--fast), color var(--fast);
    }
    .activity-button .ui-icon { width: 23px; height: 23px; stroke-width: 1.55; }
    button:hover > .ui-icon { transform: scale(1.06); }

    .side-panel {
      position: relative;
      display: flex;
      min-width: 0;
      min-height: 0;
      flex-direction: column;
      border-right: 1px solid var(--line);
      background: var(--panel);
    }
    .side-topbar {
      position: relative;
      display: flex;
      align-items: center;
      justify-content: space-between;
      min-height: 56px;
      padding: 0 13px;
    }
    .side-title {
      overflow: hidden;
      color: #f0f3f8;
      font-size: 10px;
      font-weight: 760;
      letter-spacing: 1px;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .side-meta {
      margin-top: 4px;
      color: var(--text-muted);
      font-size: 11px;
    }
    .side-actions {
      display: flex;
      gap: 6px;
    }
    .side-actions.is-hidden, .side-searchbar.is-hidden { display: none; }
    .icon-button, .toolbar-button, .tool-action {
      border: 1px solid transparent;
      border-radius: 6px;
      color: #b7bfce;
      background: transparent;
      cursor: pointer;
      transition: color var(--fast), border-color var(--fast), background var(--fast), transform var(--fast), box-shadow var(--fast);
    }
    .icon-button {
      display: grid;
      width: 24px;
      height: 24px;
      padding: 0;
      place-items: center;
    }
    .icon-button:hover, .toolbar-button:hover, .tool-action:hover {
      color: #f3f7fc;
      border-color: rgba(230, 235, 246, .12);
      background: rgba(230, 235, 246, .065);
      transform: translateY(-1px);
    }
    .icon-button:active, .toolbar-button:active, .tool-action:active { transform: scale(.97); }
    .side-action-menu {
      position: absolute;
      top: 46px;
      right: 10px;
      z-index: 8;
      display: none;
      width: 162px;
      padding: 5px;
      border: 1px solid var(--line-strong);
      border-radius: 8px;
      background: #22242d;
      box-shadow: var(--shadow);
      transform-origin: 95% 0;
    }
    .side-action-menu.is-open {
      display: grid;
      gap: 2px;
      animation: menuIn var(--slow) both;
    }
    .menu-item {
      min-height: 28px;
      border: 0;
      border-radius: 5px;
      color: #c9d0dc;
      background: transparent;
      cursor: pointer;
      font-size: 12px;
      text-align: left;
      transition: color var(--fast), background var(--fast), padding-left var(--fast);
    }
    .menu-item:hover {
      color: #f1f5fb;
      background: rgba(230, 235, 246, .07);
      padding-left: 12px;
    }
    .side-searchbar {
      padding: 0 13px 12px;
    }
    .search-wrap { position: relative; }
    .search-wrap .ui-icon {
      position: absolute;
      left: 10px;
      top: 50%;
      width: 14px;
      height: 14px;
      color: #838c9d;
      transform: translateY(-50%);
    }
    .side-searchbar input {
      width: 100%;
      height: 32px;
      padding: 0 10px 0 32px;
      border: 1px solid rgba(230, 235, 246, .08);
      border-radius: 6px;
      color: #dce2eb;
      background: #22242c;
      font-size: 12px;
      transition: border-color var(--fast), background var(--fast), box-shadow var(--fast);
    }
    .side-searchbar input:focus {
      border-color: rgba(55, 198, 133, .42);
      background: #252832;
    }
    .side-content {
      flex: 1;
      min-height: 0;
      overflow: auto;
      padding: 0 10px 12px;
      scrollbar-width: thin;
      scrollbar-color: rgba(133, 142, 160, .55) transparent;
    }
    .side-content::-webkit-scrollbar, .tool-body::-webkit-scrollbar { width: 8px; }
    .side-content::-webkit-scrollbar-thumb, .tool-body::-webkit-scrollbar-thumb {
      border: 2px solid transparent;
      border-radius: 99px;
      background: rgba(133, 142, 160, .55);
      background-clip: padding-box;
    }

    .tree-row {
      position: relative;
      display: grid;
      grid-template-columns: 16px 18px minmax(0, 1fr) 10px;
      gap: 6px;
      align-items: center;
      width: 100%;
      min-height: 28px;
      padding: 0 8px 0 calc(8px + var(--depth) * 16px);
      border: 1px solid transparent;
      border-radius: 5px;
      color: #c2c9d6;
      background: transparent;
      cursor: pointer;
      font-size: 12px;
      text-align: left;
      transition: color var(--fast), background var(--fast), box-shadow var(--fast), transform var(--fast);
      animation: surfaceIn var(--slow) both;
    }
    .tree-row:hover, .search-result:hover, .function-row:hover, .outline-row:hover, .problem-row:hover, .recent-project:hover {
      color: #f0f4fa;
      background: rgba(230, 235, 246, .06);
      transform: translateX(2px);
    }
    .tree-row.is-active {
      color: #f1f6fb;
      background: rgba(55, 198, 133, .13);
      box-shadow: inset 2px 0 0 var(--accent);
    }
    .tree-label, .document-tab-name {
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .tree-folder .chevron { width: 13px; height: 13px; transform: rotate(90deg); }
    .tree-folder.is-collapsed .chevron { transform: rotate(0); }
    .tree-spacer { width: 13px; }
    .dirty-dot {
      display: none;
      width: 6px;
      height: 6px;
      border-radius: 99px;
      background: var(--accent);
      box-shadow: 0 0 12px rgba(55, 198, 133, .38);
    }
    .is-dirty > .dirty-dot, .document-tab.is-dirty .dirty-dot { display: block; }

    .panel-empty, .tool-empty {
      display: grid;
      gap: 8px;
      padding: 16px 8px;
      color: var(--text-muted);
      font-size: 12px;
      line-height: 1.45;
      animation: surfaceIn var(--slow) both;
    }
    .panel-empty strong {
      color: var(--text-soft);
      font-size: 10px;
      letter-spacing: 1px;
    }
    .search-result, .function-row, .outline-row, .problem-row, .recent-project {
      display: grid;
      width: 100%;
      gap: 4px;
      min-height: 46px;
      padding: 8px 9px;
      border: 1px solid transparent;
      border-radius: 6px;
      color: #c8cfdb;
      background: transparent;
      cursor: pointer;
      font-size: 12px;
      text-align: left;
      transition: color var(--fast), background var(--fast), transform var(--fast);
      animation: surfaceIn var(--slow) both;
    }
    .search-result strong, .function-row strong, .outline-row strong, .recent-project strong {
      overflow: hidden;
      color: #edf2f8;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .search-result span, .function-row span, .outline-row span, .recent-project span {
      overflow: hidden;
      color: var(--text-muted);
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .search-result code {
      overflow: hidden;
      color: #bfc8da;
      font-family: "Cascadia Code", Consolas, monospace;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .side-section + .side-section { margin-top: 14px; }
    .side-section-title {
      margin: 8px 5px;
      color: var(--text-soft);
      font-size: 10px;
      font-weight: 760;
      letter-spacing: 1px;
      text-transform: uppercase;
    }
    .outline-row {
      grid-template-columns: 28px minmax(0, 1fr) auto;
      min-height: 32px;
      align-items: center;
    }
    .outline-kind {
      display: grid;
      height: 18px;
      place-items: center;
      border-radius: 4px;
      color: #111714;
      background: var(--accent);
      font-size: 10px;
      font-weight: 800;
    }

    .settings-section {
      display: grid;
      gap: 12px;
      padding: 6px 4px 20px;
      animation: surfaceIn var(--slow) both;
    }
    .settings-field, .settings-toggle {
      display: grid;
      gap: 7px;
      color: var(--text-soft);
      font-size: 12px;
      font-weight: 640;
    }
    .settings-toggle {
      grid-template-columns: 18px 1fr;
      align-items: center;
    }
    .settings-field select, .settings-field textarea {
      width: 100%;
      border: 1px solid rgba(230,235,246,.09);
      border-radius: 6px;
      color: #dce2eb;
      background: #242731;
      font-size: 12px;
    }
    .settings-field select { height: 32px; padding: 0 9px; }
    .settings-field textarea {
      min-height: 74px;
      padding: 8px 9px;
      resize: vertical;
    }
    .capability-cloud {
      display: flex;
      flex-wrap: wrap;
      gap: 6px;
    }
    .capability-chip {
      min-height: 25px;
      border: 1px solid rgba(230,235,246,.09);
      border-radius: 5px;
      color: #aeb7c8;
      background: #22242d;
      cursor: pointer;
      font-size: 11px;
      transition: color var(--fast), border-color var(--fast), background var(--fast), transform var(--fast);
    }
    .capability-chip:hover, .capability-chip.is-active {
      color: #101713;
      border-color: transparent;
      background: var(--accent);
      transform: translateY(-1px);
    }

    .editor-surface {
      display: grid;
      grid-template-rows: 35px 40px 1fr var(--tool-height);
      min-width: 0;
      min-height: 0;
      background: #18191e;
    }
    .menu-strip {
      display: flex;
      align-items: center;
      gap: 4px;
      padding: 0 10px;
      border-bottom: 1px solid var(--line);
      background: #17181e;
    }
    .menu-strip-label {
      margin-right: 8px;
      color: #f0f3f8;
      font-size: 12px;
      font-weight: 720;
    }
    .toolbar-button {
      min-height: 25px;
      padding: 0 9px;
      font-size: 12px;
    }
    .toolbar-button-primary {
      color: #101713;
      border-color: transparent;
      background: linear-gradient(135deg, var(--accent), var(--accent-blue));
      font-weight: 760;
    }
    .toolbar-separator {
      width: 1px;
      height: 18px;
      margin: 0 5px;
      background: var(--line-strong);
    }
    .runtime-controls {
      display: flex;
      align-items: center;
      gap: 8px;
      min-width: 0;
      margin-left: auto;
      color: var(--text-muted);
      font-size: 11px;
    }
    .runtime-controls select, .runtime-controls input[type="text"] {
      height: 26px;
      border: 1px solid rgba(230,235,246,.09);
      border-radius: 5px;
      color: #dce2eb;
      background: #22242d;
      font-size: 11px;
    }
    .runtime-controls select { width: 76px; padding: 0 7px; }
    .runtime-controls input[type="text"] { width: min(280px, 22vw); padding: 0 8px; }
    .runtime-toggle {
      display: flex;
      align-items: center;
      gap: 5px;
      white-space: nowrap;
    }

    .document-tabs {
      display: flex;
      min-width: 0;
      overflow-x: auto;
      border-bottom: 1px solid var(--line);
      background: #17181e;
      scrollbar-width: none;
    }
    .document-tabs::-webkit-scrollbar { display: none; }
    .document-tab {
      display: grid;
      grid-template-columns: 16px minmax(50px, 1fr) 8px 16px;
      gap: 8px;
      align-items: center;
      min-width: 132px;
      max-width: 230px;
      height: 39px;
      padding: 0 10px;
      border: 0;
      border-right: 1px solid var(--line);
      color: #acb5c5;
      background: #17181e;
      cursor: pointer;
      font-size: 12px;
      text-align: left;
      transition: color var(--fast), background var(--fast);
    }
    .document-tab:hover {
      color: #f0f4fa;
      background: #1f2129;
    }
    .document-tab.is-active {
      color: #f4f7fb;
      background: #18191e;
      box-shadow: inset 0 -2px 0 var(--accent);
    }
    .document-tab.is-empty {
      display: flex;
      cursor: default;
    }
    .tab-close {
      display: grid;
      width: 16px;
      height: 16px;
      place-items: center;
      border-radius: 4px;
      color: #8790a2;
      font-size: 12px;
      line-height: 1;
      transition: color var(--fast), background var(--fast);
    }
    .tab-close:hover {
      color: #f0f4fa;
      background: rgba(230,235,246,.09);
    }

    .editor-workbench {
      position: relative;
      min-width: 0;
      min-height: 0;
      overflow: hidden;
      background:
        linear-gradient(135deg, rgba(55,198,133,.035), transparent 36%),
        #18191e;
    }
    #monacoEditor {
      position: absolute;
      inset: 0;
      opacity: 1;
      transition: opacity var(--med);
    }
    #monacoEditor.is-start-hidden {
      opacity: 0;
      pointer-events: none;
    }
    .start-surface {
      position: absolute;
      inset: 0;
      z-index: 2;
      display: none;
      align-items: center;
      justify-content: center;
      overflow: auto;
      padding: 34px;
      background: linear-gradient(135deg, rgba(24,25,30,.98), rgba(20,21,26,.99));
    }
    .start-surface.is-visible {
      display: flex;
      animation: surfaceIn var(--slow) both;
    }
    .start-shell {
      display: grid;
      grid-template-columns: minmax(260px, .8fr) minmax(320px, 1fr);
      gap: 44px;
      width: min(940px, 100%);
      align-items: start;
    }
    .start-mark {
      display: grid;
      width: 52px;
      height: 52px;
      place-items: center;
      border-radius: 10px;
      color: #101713;
      background: linear-gradient(135deg, var(--accent), var(--accent-blue));
      box-shadow: 0 22px 60px rgba(55,198,133,.18);
      font-size: 21px;
      font-weight: 850;
    }
    .start-title {
      margin: 20px 0 0;
      color: #f4f7fb;
      font-size: 34px;
      font-weight: 760;
      letter-spacing: 0;
      line-height: 1.08;
    }
    .start-subtitle {
      max-width: 360px;
      margin: 12px 0 28px;
      color: #a2acbd;
      font-size: 13px;
      line-height: 1.6;
    }
    .recent-title {
      margin: 0 0 8px;
      color: var(--text-soft);
      font-size: 10px;
      font-weight: 760;
      letter-spacing: 1px;
    }
    .recent-list {
      display: grid;
      gap: 6px;
    }
    .recent-project {
      min-height: 48px;
      border-radius: 7px;
      background: rgba(255,255,255,.018);
    }
    .recent-empty {
      color: var(--text-muted);
      font-size: 12px;
    }
    .start-actions-panel {
      display: grid;
      gap: 10px;
    }
    .start-action {
      display: grid;
      grid-template-columns: 42px 1fr auto;
      gap: 14px;
      align-items: center;
      width: 100%;
      min-height: 76px;
      padding: 14px 16px;
      border: 1px solid rgba(230,235,246,.09);
      border-radius: 8px;
      color: #dfe5ee;
      background: linear-gradient(180deg, rgba(255,255,255,.028), rgba(255,255,255,.006)), #20222a;
      cursor: pointer;
      text-align: left;
      transition: border-color var(--fast), background var(--fast), transform var(--fast), box-shadow var(--med);
    }
    .start-action:hover {
      border-color: rgba(55,198,133,.35);
      background: #242731;
      box-shadow: var(--shadow);
      transform: translateY(-2px);
    }
    .start-action:active { transform: scale(.99); }
    .start-action-icon {
      display: grid;
      width: 42px;
      height: 42px;
      place-items: center;
      border-radius: 7px;
      color: var(--accent);
      background: rgba(55,198,133,.11);
    }
    .start-action-primary .start-action-icon {
      color: #101713;
      background: linear-gradient(135deg, var(--accent), var(--accent-blue));
    }
    .start-action-title {
      display: block;
      color: #f4f7fb;
      font-size: 14px;
      font-weight: 720;
    }
    .start-action-note {
      display: block;
      margin-top: 5px;
      color: #98a2b3;
      font-size: 12px;
    }
    .start-action-arrow { color: #8993a4; }
    .start-action:hover .start-action-arrow { transform: translateX(3px); color: #eef2f8; }

    .tool-window {
      display: grid;
      grid-template-rows: 34px 1fr;
      min-height: 0;
      border-top: 1px solid var(--line);
      background: #15161b;
    }
    .is-start-mode + .tool-window, .editor-workbench.is-start-mode ~ .tool-window {
      display: none;
    }
    .tool-tabs {
      display: flex;
      align-items: center;
      gap: 2px;
      padding: 0 8px;
      border-bottom: 1px solid var(--line);
      background: #17181e;
    }
    .tool-tab {
      height: 28px;
      padding: 0 10px;
      border: 0;
      border-radius: 5px;
      color: #8f98aa;
      background: transparent;
      cursor: pointer;
      font-size: 11px;
      font-weight: 680;
      letter-spacing: .2px;
      transition: color var(--fast), background var(--fast), transform var(--fast);
    }
    .tool-tab:hover, .tool-tab.is-active {
      color: #eef2f8;
      background: rgba(230,235,246,.07);
      transform: translateY(-1px);
    }
    .tool-body {
      min-height: 0;
      overflow: auto;
      padding: 8px 10px 12px;
      font-family: "Cascadia Code", "SF Mono", Consolas, monospace;
      font-size: 12px;
      line-height: 1.55;
    }
    .log-line {
      display: grid;
      grid-template-columns: 72px 1fr;
      gap: 10px;
      min-height: 22px;
      color: #c5ccd8;
      white-space: pre-wrap;
      animation: surfaceIn var(--slow) both;
    }
    .log-time { color: #697386; }
    .log-error { color: #f0a0a0; }
    .log-success { color: #8ce0b5; }
    .log-warning { color: #e3c482; }
    .log-command { color: #a7c3ff; }
    .problem-row {
      grid-template-columns: 68px minmax(0, 1fr) auto;
      min-height: 32px;
      align-items: center;
      font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text", "Segoe UI", sans-serif;
    }
    .problem-severity {
      display: grid;
      height: 20px;
      place-items: center;
      border-radius: 4px;
      color: #1c0f0f;
      background: var(--danger);
      font-size: 10px;
      font-weight: 800;
      text-transform: uppercase;
    }
    .terminal-tool {
      display: grid;
      grid-template-rows: 1fr 34px;
      height: 100%;
      min-height: 0;
    }
    .terminal-log {
      min-height: 0;
      overflow: auto;
    }
    .terminal-input-row {
      display: grid;
      grid-template-columns: auto 1fr auto;
      gap: 8px;
      align-items: center;
      padding-top: 6px;
    }
    .terminal-input-row input {
      height: 28px;
      border: 1px solid rgba(230,235,246,.09);
      border-radius: 6px;
      color: #dce2eb;
      background: #22242d;
      font-family: "Cascadia Code", Consolas, monospace;
      font-size: 12px;
      padding: 0 9px;
    }
    .tool-action {
      min-height: 28px;
      padding: 0 10px;
      font-size: 12px;
    }
    .tool-action-primary {
      color: #101713;
      border-color: transparent;
      background: var(--accent);
      font-weight: 760;
    }

    .status-bar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      min-width: 0;
      padding: 0 9px;
      border: 1px solid var(--line);
      border-top-color: var(--line-strong);
      color: #9da6b7;
      background: #17181e;
      font-size: 11px;
    }
    .status-left, .status-right {
      display: flex;
      align-items: center;
      gap: 13px;
      min-width: 0;
    }
    .status-item {
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .status-button {
      height: 100%;
      padding: 0;
      border: 0;
      color: inherit;
      background: transparent;
      cursor: pointer;
      font: inherit;
      transition: color var(--fast), transform var(--fast);
    }
    .status-button:hover { color: #edf2f8; transform: translateY(-1px); }
    .status-item.is-live { animation: livePulse 900ms ease-out both; }
    #statusDirty.is-dirty { color: var(--warning); }
    .status-dot {
      width: 7px;
      height: 7px;
      border-radius: 99px;
      background: var(--accent);
      box-shadow: 0 0 10px rgba(55,198,133,.38);
    }

    .project-dialog {
      position: fixed;
      inset: 0;
      z-index: 30;
      display: none;
      align-items: center;
      justify-content: center;
      padding: 26px;
      background: rgba(6, 7, 10, .62);
      backdrop-filter: blur(14px);
    }
    .project-dialog.is-open {
      display: flex;
      animation: surfaceIn var(--slow) both;
    }
    .project-dialog-card {
      width: min(620px, 100%);
      padding: 20px;
      border: 1px solid rgba(230,235,246,.1);
      border-radius: 9px;
      background: #1f2129;
      box-shadow: var(--shadow);
    }
    .project-dialog-card h2 {
      margin: 0 0 15px;
      color: #f4f7fb;
      font-size: 18px;
      font-weight: 760;
    }
    .project-field {
      display: grid;
      gap: 8px;
      color: var(--text-soft);
      font-size: 12px;
      font-weight: 680;
    }
    #newProjectName {
      height: 36px;
      padding: 0 11px;
      border: 1px solid rgba(230,235,246,.1);
      border-radius: 6px;
      color: #e4e9f2;
      background: #262934;
    }
    .template-picker {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 9px;
      margin-top: 14px;
    }
    .template-card {
      display: grid;
      gap: 6px;
      min-height: 92px;
      padding: 11px;
      border: 1px solid rgba(230,235,246,.09);
      border-radius: 8px;
      color: #c8cfdb;
      background: #242731;
      cursor: pointer;
      text-align: left;
      transition: border-color var(--fast), background var(--fast), transform var(--fast), box-shadow var(--fast);
    }
    .template-card:hover, .template-card.is-active {
      border-color: rgba(55,198,133,.42);
      background: #292d38;
      transform: translateY(-1px);
    }
    .template-card.is-active { box-shadow: inset 0 0 0 1px rgba(55,198,133,.18); }
    .template-card strong { color: #f1f5fb; font-size: 13px; }
    .template-card span { color: #98a2b3; font-size: 12px; line-height: 1.4; }
    .project-dialog-actions {
      display: flex;
      justify-content: flex-end;
      gap: 9px;
      margin-top: 18px;
    }
    .dialog-button {
      min-width: 86px;
      height: 32px;
      border: 1px solid rgba(230,235,246,.1);
      border-radius: 6px;
      color: #cbd3df;
      background: #252832;
      cursor: pointer;
      font-size: 12px;
      transition: border-color var(--fast), background var(--fast), transform var(--fast);
    }
    .dialog-button:hover {
      border-color: rgba(230,235,246,.18);
      background: #2b2f3a;
      transform: translateY(-1px);
    }
    .dialog-button-primary {
      border-color: transparent;
      color: #101713;
      background: linear-gradient(135deg, var(--accent), var(--accent-blue));
      font-weight: 760;
    }

    @media (max-width: 980px) {
      :root { --side-width: 240px; --activity-width: 50px; --tool-height: 170px; }
      .runtime-controls input[type="text"] { display: none; }
      .start-shell { grid-template-columns: 1fr; gap: 28px; }
      .template-picker { grid-template-columns: 1fr; }
    }
    @media (max-width: 760px) {
      .workspace { grid-template-columns: var(--activity-width) 1fr; }
      .side-panel { display: none; }
      .editor-surface { grid-template-rows: auto 40px 1fr var(--tool-height); }
      .menu-strip { flex-wrap: wrap; min-height: 70px; padding-block: 6px; }
      .runtime-controls { width: 100%; margin-left: 0; }
      .runtime-toggle span { display: none; }
      .start-surface { align-items: flex-start; padding: 22px; }
    }
    @media (prefers-reduced-motion: reduce) {
      *, *::before, *::after {
        animation-duration: 1ms !important;
        animation-iteration-count: 1 !important;
        scroll-behavior: auto !important;
        transition-duration: 1ms !important;
      }
    }
  `;
  style.textContent += `
    :root {
      color-scheme: dark;
      font-family: "Segoe UI", -apple-system, BlinkMacSystemFont, sans-serif;
      --chrome-height: 34px;
      --status-height: 22px;
      --activity-width: 48px;
      --side-width: 292px;
      --tool-height: 196px;
      --accent: #007acc;
      --accent-blue: #007acc;
      --danger: #f48771;
      --warning: #cca700;
      --bg: #1e1e1e;
      --panel: #252526;
      --panel-2: #2d2d30;
      --panel-3: #333333;
      --line: #3c3c3c;
      --line-strong: #464647;
      --text: #cccccc;
      --text-soft: #c5c5c5;
      --text-muted: #858585;
      --shadow: 0 8px 18px rgba(0, 0, 0, .32);
      --focus: 0 0 0 1px #007acc;
      --fast: 80ms ease;
      --med: 100ms ease;
      --slow: 120ms ease;
    }

    * { letter-spacing: 0 !important; }
    body { background: #1e1e1e; }
    button, input, select, textarea { font-family: "Segoe UI", -apple-system, BlinkMacSystemFont, sans-serif; }
    .app-shell, .workspace, .editor-workbench, .start-surface {
      background: #1e1e1e !important;
    }
    #windowChrome {
      grid-template-columns: 240px 1fr 138px;
      border: 0;
      border-bottom: 1px solid var(--line);
      background: #323233;
    }
    #windowBannerFrame {
      width: 82px;
      height: 20px;
      margin-left: 10px;
    }
    #windowBannerIcon {
      top: -6px;
      height: 32px;
      opacity: .9;
    }
    #windowAppTitle {
      justify-self: center;
      color: #cccccc;
      font-size: 12px;
      font-weight: 400;
    }
    #windowButtons {
      height: 100%;
      gap: 0;
      padding-right: 0;
    }
    .window-btn {
      width: 46px;
      height: 100%;
      border-radius: 0;
      color: #cccccc;
    }
    .window-btn:hover {
      color: #ffffff;
      background: #3e3e42;
      transform: none;
    }
    .window-btn-close:hover { background: #c42b1c; }
    .window-btn:active, .icon-button:active, .toolbar-button:active, .tool-action:active, .start-action:active {
      transform: none;
    }
    .window-icon, .window-btn .ui-icon {
      position: static;
      width: 13px;
      height: 13px;
      margin: auto;
      transform: none;
    }
    .window-icon::before, .window-icon::after { display: none; }

    .activity-bar {
      gap: 0;
      padding: 0;
      border-right: 1px solid var(--line);
      background: #333333;
    }
    .activity-button {
      width: 48px;
      height: 48px;
      margin: 0;
      border-radius: 0;
      color: #c5c5c5;
      transition: background var(--fast), color var(--fast);
    }
    .activity-button::before {
      left: 0;
      width: 2px;
      height: 100%;
      border-radius: 0;
      background: #ffffff;
      transform: translateY(-50%);
    }
    .activity-button:hover {
      color: #ffffff;
      background: #3e3e42;
      transform: none;
    }
    .activity-button.is-active {
      color: #ffffff;
      background: #2d2d30;
      box-shadow: none;
    }
    .activity-button .ui-icon {
      width: 22px;
      height: 22px;
      stroke-width: 1.8;
    }
    button:hover > .ui-icon, .start-action:hover .start-action-arrow {
      transform: none;
    }

    .ui-icon {
      width: 16px;
      height: 16px;
      flex: 0 0 auto;
      stroke-width: 1.8;
    }
    .side-panel {
      border-right: 1px solid var(--line);
      background: #252526;
    }
    .side-topbar {
      min-height: 35px;
      padding: 0 8px 0 12px;
      border-bottom: 1px solid var(--line);
      background: #252526;
    }
    .side-title {
      color: #cccccc;
      font-size: 11px;
      font-weight: 600;
      text-transform: uppercase;
    }
    .side-meta {
      display: none;
    }
    .side-actions { gap: 0; }
    .icon-button {
      width: 26px;
      height: 26px;
      border-radius: 2px;
      color: #c5c5c5;
    }
    .icon-button:hover, .toolbar-button:hover, .tool-action:hover {
      color: #ffffff;
      border-color: transparent;
      background: #37373d;
      transform: none;
      box-shadow: none;
    }
    .side-action-menu {
      top: 31px;
      right: 8px;
      width: 174px;
      padding: 2px;
      border-color: var(--line-strong);
      border-radius: 2px;
      background: #252526;
    }
    .side-action-menu.is-open { animation: none; }
    .menu-item {
      min-height: 26px;
      border-radius: 0;
      color: #cccccc;
      font-size: 12px;
    }
    .menu-item:hover {
      color: #ffffff;
      background: #094771;
      padding-left: 6px;
    }
    .side-searchbar {
      padding: 8px;
      border-bottom: 1px solid var(--line);
    }
    .side-searchbar input, .runtime-controls select, .runtime-controls input[type="text"],
    .settings-field select, .settings-field textarea, #newProjectName,
    .terminal-input-row input {
      border: 1px solid #3c3c3c;
      border-radius: 2px;
      color: #cccccc;
      background: #1e1e1e;
      font-size: 12px;
      box-shadow: none;
    }
    .side-searchbar input:focus, .runtime-controls select:focus, .runtime-controls input[type="text"]:focus,
    .settings-field select:focus, .settings-field textarea:focus, #newProjectName:focus,
    .terminal-input-row input:focus {
      border-color: #007acc;
      background: #1e1e1e;
    }
    .side-searchbar input { height: 28px; padding-left: 30px; }
    .side-content {
      padding: 6px 0 10px;
      scrollbar-color: #5a5a5a transparent;
    }
    .tree-row {
      grid-template-columns: 14px 16px minmax(0, 1fr) 10px;
      gap: 5px;
      min-height: 24px;
      padding: 0 8px 0 calc(8px + var(--depth) * 14px);
      border: 0;
      border-radius: 0;
      color: #cccccc;
      font-size: 12px;
      animation: none;
      transition: background var(--fast), color var(--fast);
    }
    .tree-row:hover, .search-result:hover, .function-row:hover, .outline-row:hover, .problem-row:hover, .recent-project:hover {
      color: #ffffff;
      background: #2a2d2e;
      transform: none;
    }
    .tree-row.is-active {
      color: #ffffff;
      background: #37373d;
      box-shadow: inset 2px 0 0 #007acc;
    }
    .tree-folder .chevron {
      width: 14px;
      height: 14px;
    }
    .dirty-dot {
      width: 7px;
      height: 7px;
      background: #ffffff;
      box-shadow: none;
    }
    .panel-empty, .tool-empty {
      padding: 10px 12px;
      color: #858585;
      font-size: 12px;
      animation: none;
    }
    .panel-empty strong {
      color: #cccccc;
      font-size: 11px;
      text-transform: uppercase;
    }
    .search-result, .function-row, .outline-row, .problem-row, .recent-project {
      min-height: 34px;
      padding: 6px 12px;
      border: 0;
      border-radius: 0;
      color: #cccccc;
      font-size: 12px;
      animation: none;
    }
    .search-result strong, .function-row strong, .outline-row strong, .recent-project strong {
      color: #cccccc;
      font-weight: 600;
    }
    .side-section + .side-section { margin-top: 8px; }
    .side-section-title {
      margin: 10px 12px 4px;
      color: #858585;
      font-size: 11px;
      font-weight: 600;
      text-transform: uppercase;
    }
    .outline-kind {
      height: 18px;
      border-radius: 2px;
      color: #ffffff;
      background: #007acc;
      font-size: 10px;
    }
    .settings-section {
      gap: 10px;
      padding: 10px 12px 16px;
      animation: none;
    }
    .settings-field, .settings-toggle {
      gap: 6px;
      color: #cccccc;
      font-size: 12px;
      font-weight: 400;
    }
    .capability-cloud { gap: 4px; }
    .capability-chip {
      min-height: 22px;
      border-color: #3c3c3c;
      border-radius: 2px;
      color: #cccccc;
      background: #2d2d30;
      font-size: 11px;
    }
    .capability-chip:hover, .capability-chip.is-active {
      color: #ffffff;
      border-color: #007acc;
      background: #094771;
      transform: none;
    }

    .editor-surface {
      grid-template-rows: 34px 35px 1fr var(--tool-height);
      background: #1e1e1e;
    }
    .menu-strip {
      gap: 2px;
      padding: 0 8px;
      border-bottom: 1px solid var(--line);
      background: #2d2d30;
    }
    .menu-strip-label {
      min-width: 46px;
      margin-right: 6px;
      color: #cccccc;
      font-size: 12px;
      font-weight: 600;
    }
    .toolbar-button, .tool-action, .dialog-button {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 6px;
      min-height: 26px;
      padding: 0 8px;
      border: 1px solid transparent;
      border-radius: 2px;
      color: #cccccc;
      background: transparent;
      font-size: 12px;
      font-weight: 400;
      line-height: 1;
    }
    .toolbar-button-primary, .tool-action-primary, .dialog-button-primary {
      color: #ffffff;
      border-color: #0e639c;
      background: #0e639c;
      font-weight: 400;
    }
    .toolbar-button-primary:hover, .tool-action-primary:hover, .dialog-button-primary:hover {
      background: #1177bb;
    }
    .toolbar-separator {
      height: 20px;
      margin: 0 6px;
      background: #3c3c3c;
    }
    .runtime-controls {
      gap: 8px;
      color: #cccccc;
      font-size: 12px;
    }
    .runtime-controls select { width: 78px; height: 26px; }
    .runtime-controls input[type="text"] { height: 26px; width: min(320px, 24vw); }
    .runtime-toggle { gap: 5px; }

    .document-tabs {
      border-bottom: 1px solid var(--line);
      background: #252526;
    }
    .document-tab {
      grid-template-columns: 16px minmax(56px, 1fr) 8px 18px;
      gap: 7px;
      min-width: 132px;
      max-width: 230px;
      height: 34px;
      padding: 0 8px;
      border-right: 1px solid #1e1e1e;
      color: #cccccc;
      background: #2d2d30;
      font-size: 12px;
      transition: background var(--fast), color var(--fast);
    }
    .document-tab:hover {
      color: #ffffff;
      background: #333337;
    }
    .document-tab.is-active {
      color: #ffffff;
      background: #1e1e1e;
      box-shadow: inset 0 1px 0 #007acc;
    }
    .tab-close {
      width: 18px;
      height: 18px;
      border-radius: 2px;
      font-size: 0;
    }
    .tab-close .ui-icon {
      width: 13px;
      height: 13px;
    }
    .tab-close:hover { background: #3e3e42; }

    #monacoEditor { transition: none; }
    .start-surface {
      align-items: flex-start;
      justify-content: flex-start;
      padding: 36px 48px;
      background: #1e1e1e !important;
    }
    .start-surface.is-visible { animation: none; }
    .start-shell {
      grid-template-columns: minmax(260px, 360px) minmax(300px, 460px);
      gap: 36px;
      width: min(900px, 100%);
    }
    .start-mark {
      display: none;
    }
    .start-title {
      margin: 0 0 10px;
      color: #ffffff;
      font-size: 28px;
      font-weight: 400;
      line-height: 1.2;
    }
    .start-subtitle {
      max-width: 520px;
      margin: 0 0 28px;
      color: #cccccc;
      font-size: 13px;
      line-height: 1.5;
    }
    .recent-title {
      color: #858585;
      font-size: 11px;
      font-weight: 600;
      text-transform: uppercase;
    }
    .start-actions-panel {
      gap: 8px;
      align-self: start;
      padding-top: 4px;
    }
    .start-action {
      grid-template-columns: 28px 1fr 18px;
      gap: 10px;
      min-height: 58px;
      padding: 8px 10px;
      border: 1px solid #3c3c3c;
      border-radius: 2px;
      color: #cccccc;
      background: #252526;
      box-shadow: none;
      animation: none;
    }
    .start-action:hover {
      border-color: #007acc;
      color: #ffffff;
      background: #2a2d2e;
      transform: none;
      box-shadow: none;
    }
    .start-action-primary {
      border-color: #007acc;
      background: #252526;
    }
    .start-action-icon {
      width: 28px;
      height: 28px;
      border: 0;
      border-radius: 2px;
      color: #cccccc;
      background: #333333;
    }
    .start-action-primary .start-action-icon {
      color: #ffffff;
      background: #0e639c;
    }
    .start-action-title {
      color: #ffffff;
      font-size: 13px;
      font-weight: 600;
    }
    .start-action-note {
      color: #858585;
      font-size: 12px;
    }

    .tool-window {
      grid-template-rows: 30px 1fr;
      border-top: 1px solid var(--line);
      background: #1e1e1e;
    }
    .tool-tabs {
      gap: 0;
      padding: 0;
      border-bottom: 1px solid var(--line);
      background: #252526;
    }
    .tool-tab {
      min-width: 92px;
      height: 30px;
      padding: 0 12px;
      border: 0;
      border-right: 1px solid #1e1e1e;
      border-radius: 0;
      color: #cccccc;
      background: #2d2d30;
      font-size: 12px;
    }
    .tool-tab:hover, .tool-tab.is-active {
      color: #ffffff;
      background: #1e1e1e;
      box-shadow: inset 0 1px 0 #007acc;
    }
    .tool-body {
      padding: 6px 8px;
      background: #1e1e1e;
      font-size: 12px;
    }
    .log-line {
      grid-template-columns: 70px minmax(0, 1fr);
      min-height: 20px;
      padding: 0 2px;
      color: #cccccc;
      font-family: "Cascadia Code", Consolas, monospace;
      font-size: 12px;
    }
    .log-time { color: #858585; }
    .problem-row {
      grid-template-columns: 64px minmax(0, 1fr) auto;
      min-height: 28px;
      padding: 4px 6px;
    }
    .problem-severity {
      border-radius: 2px;
      color: #ffffff;
      background: #a1260d;
      font-size: 10px;
    }
    .terminal-tool { grid-template-rows: 1fr 32px; }
    .terminal-input-row {
      gap: 6px;
      padding-top: 5px;
      border-top: 1px solid #2d2d30;
    }
    .terminal-input-row input { height: 26px; }

    .status-bar {
      padding: 0 8px;
      border: 0;
      color: #ffffff;
      background: #007acc;
      font-size: 12px;
    }
    .status-left, .status-right { gap: 12px; }
    .status-button {
      display: inline-flex;
      align-items: center;
      gap: 5px;
      height: 100%;
      color: #ffffff;
    }
    .status-button .ui-icon {
      width: 14px;
      height: 14px;
    }
    .status-button:hover {
      color: #ffffff;
      background: rgba(255, 255, 255, .14);
      transform: none;
    }
    .status-dot {
      width: 7px;
      height: 7px;
      background: #ffffff;
      box-shadow: none;
    }

    .project-dialog {
      background: rgba(0, 0, 0, .46);
      backdrop-filter: none;
    }
    .project-dialog.is-open { animation: none; }
    .project-dialog-card {
      padding: 18px;
      border: 1px solid #454545;
      border-radius: 2px;
      background: #252526;
      box-shadow: var(--shadow);
    }
    .project-dialog-card h2 {
      color: #ffffff;
      font-size: 18px;
      font-weight: 400;
    }
    .project-field {
      color: #cccccc;
      font-size: 12px;
      font-weight: 400;
    }
    .template-picker { gap: 8px; }
    .template-card {
      min-height: 84px;
      padding: 10px;
      border-color: #3c3c3c;
      border-radius: 2px;
      color: #cccccc;
      background: #2d2d30;
      box-shadow: none;
    }
    .template-card:hover, .template-card.is-active {
      border-color: #007acc;
      background: #333337;
      transform: none;
    }
    .template-card.is-active { box-shadow: inset 0 0 0 1px #007acc; }
    .template-card strong { color: #ffffff; font-size: 13px; }
    .template-card span { color: #a0a0a0; font-size: 12px; }
    .dialog-button:hover {
      border-color: transparent;
      background: #3e3e42;
      transform: none;
    }

    @media (max-width: 980px) {
      :root { --side-width: 250px; --activity-width: 48px; --tool-height: 176px; }
      .start-shell { grid-template-columns: 1fr; gap: 24px; }
    }
    @media (max-width: 760px) {
      .workspace { grid-template-columns: var(--activity-width) 1fr; }
      .editor-surface { grid-template-rows: auto 35px 1fr var(--tool-height); }
      .menu-strip { min-height: 64px; padding-block: 5px; }
      .runtime-controls { width: 100%; margin-left: 0; }
      .start-surface { padding: 24px; }
    }
  `;
  document.head.appendChild(style);
}

function renderWindowBar() {
  const root = document.getElementById("root");
  root.innerHTML = `
    <main class="app-shell">
      <header id="windowChrome">
        <div id="windowBannerFrame"><img id="windowBannerIcon" src="${bannerIconUrl}" alt="Wapi"></div>
        <div id="windowAppTitle">Wapi IDE</div>
        <div id="windowButtons">
          <button id="windowMinimize" class="window-btn" type="button" aria-label="Minimize">${iconSvg(Minus, "ui-icon window-icon")}</button>
          <button id="windowMaximize" class="window-btn" type="button" aria-label="Maximize">${iconSvg(Maximize2, "ui-icon window-icon")}</button>
          <button id="windowClose" class="window-btn window-btn-close" type="button" aria-label="Close">${iconSvg(X, "ui-icon window-icon")}</button>
        </div>
      </header>
      <section class="workspace" aria-label="Wapi IDE workspace">
        <nav class="activity-bar" aria-label="Primary navigation">
          <button class="activity-button is-active" type="button" title="Explorer" data-panel="explorer">
            ${iconSvg(PanelLeft)}
          </button>
          <button class="activity-button" type="button" title="Search" data-panel="search">
            ${iconSvg(Search)}
          </button>
          <button class="activity-button" type="button" title="Functions" data-panel="functions">
            ${iconSvg(Braces)}
          </button>
          <button class="activity-button" type="button" title="Outline" data-panel="outline">
            ${iconSvg(ListTree)}
          </button>
          <div class="activity-spacer" aria-hidden="true"></div>
          <button class="activity-button" type="button" title="Project settings" data-panel="settings">
            ${iconSvg(Settings)}
          </button>
        </nav>
        <aside class="side-panel" aria-label="Side panel">
          <div class="side-topbar">
            <div>
              <div id="sideTitle" class="side-title">SOLUTION EXPLORER</div>
              <div id="sideMeta" class="side-meta">No project loaded</div>
            </div>
            <div id="sideActions" class="side-actions">
              <button id="sideCreateProject" class="icon-button" type="button" title="Create project">${iconSvg(Plus)}</button>
              <button id="sideUploadProject" class="icon-button" type="button" title="Upload project">${iconSvg(Upload)}</button>
              <button id="sideMoreActions" class="icon-button" type="button" title="More actions">${iconSvg(Ellipsis)}</button>
            </div>
            <div id="sideActionMenu" class="side-action-menu" role="menu">
              <button class="menu-item" type="button" data-menu-action="create-project">Create new project</button>
              <button class="menu-item" type="button" data-menu-action="upload-project">Upload project</button>
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
        </aside>
        <section class="editor-surface" aria-label="Editor">
          <div class="menu-strip">
            <span class="menu-strip-label">Wapi</span>
            <button id="toolbarSave" class="toolbar-button" type="button">${iconSvg(Save)}<span>Save</span></button>
            <button id="toolbarSaveAs" class="toolbar-button" type="button">Save As</button>
            <button id="toolbarSaveAll" class="toolbar-button" type="button">${iconSvg(SaveAll)}<span>Save All</span></button>
            <span class="toolbar-separator"></span>
            <button id="toolbarCheck" class="toolbar-button toolbar-button-primary" type="button">${iconSvg(SquareCheck)}<span>Check</span></button>
            <button id="toolbarRun" class="toolbar-button" type="button">${iconSvg(Play)}<span>Run</span></button>
            <div class="runtime-controls" aria-label="Runtime controls">
              <select id="runtimeMode" title="Runtime mode">
                <option value="safe">Safe</option>
                <option value="dev">Dev</option>
                <option value="unsafe">Unsafe</option>
              </select>
              <label class="runtime-toggle"><input id="runtimeStrict" type="checkbox"><span>Strict</span></label>
              <label class="runtime-toggle"><input id="runtimeInjection" type="checkbox"><span>Injection</span></label>
              <input id="runtimeCapabilities" type="text" spellcheck="false" title="Capabilities" placeholder="capabilities">
            </div>
          </div>
          <div id="documentTabs" class="document-tabs"></div>
          <div class="editor-workbench">
            <section id="startSurface" class="start-surface" aria-label="Start">
              <div class="start-shell">
                <div class="start-identity">
                  <div class="start-mark" aria-hidden="true">W</div>
                  <h1 class="start-title">Wapi IDE</h1>
                  <p class="start-subtitle">Create a Wapi project or upload an existing folder. The workbench opens with Check as the primary runtime action.</p>
                  <div class="recent-title">RECENT PROJECTS</div>
                  <div id="recentProjects" class="recent-list"></div>
                </div>
                <div class="start-actions-panel">
                  <button id="startCreateProject" class="start-action start-action-primary" type="button">
                    <span class="start-action-icon">${iconSvg(Plus)}</span>
                    <span><span class="start-action-title">Create new project</span><span class="start-action-note">Choose Empty, Process Inspector, or Memory Sandbox</span></span>
                    ${iconSvg(ArrowRight, "ui-icon start-action-arrow")}
                  </button>
                  <button id="startUploadProject" class="start-action" type="button">
                    <span class="start-action-icon">${iconSvg(Upload)}</span>
                    <span><span class="start-action-title">Upload project</span><span class="start-action-note">Open an existing Wapi folder</span></span>
                    ${iconSvg(ArrowRight, "ui-icon start-action-arrow")}
                  </button>
                </div>
              </div>
            </section>
            <div id="monacoEditor"></div>
          </div>
          <section class="tool-window" aria-label="Tool windows">
            <div id="toolTabs" class="tool-tabs">
              <button class="tool-tab" type="button" data-tool="problems">Problems</button>
              <button class="tool-tab is-active" type="button" data-tool="output">Output</button>
              <button class="tool-tab" type="button" data-tool="terminal">Terminal</button>
              <button class="tool-tab" type="button" data-tool="audit">Audit</button>
            </div>
            <div id="toolBody" class="tool-body"></div>
          </section>
        </section>
      </section>
      <footer class="status-bar">
        <div class="status-left">
          <span id="statusFile" class="status-item">Welcome</span>
          <span id="statusDirty" class="status-item">Saved</span>
        </div>
        <div class="status-right">
          <span id="statusCursor" class="status-item">Ln 1, Col 1</span>
          <span class="status-item">Spaces: 4</span>
          <span class="status-item">UTF-8</span>
          <span id="statusLanguage" class="status-item">Wapi</span>
          <button id="statusTerminal" class="status-button" type="button">${iconSvg(Terminal)}<span>Terminal</span></button>
          <span class="status-dot" aria-hidden="true"></span>
          <span id="statusInstance" class="status-item">Ready</span>
        </div>
      </footer>
      <div id="newProjectDialog" class="project-dialog" aria-hidden="true">
        <form id="newProjectForm" class="project-dialog-card">
          <h2>Create a new project</h2>
          <label class="project-field">
            <span>Project name</span>
            <input id="newProjectName" type="text" autocomplete="off" spellcheck="false" value="WapiProject">
          </label>
          <div id="templatePicker" class="template-picker"></div>
          <div class="project-dialog-actions">
            <button id="newProjectCancel" class="dialog-button" type="button">Cancel</button>
            <button class="dialog-button dialog-button-primary" type="submit">Create</button>
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
  document.getElementById("sideCreateProject")?.addEventListener("click", openCreateProjectDialog);
  document.getElementById("sideUploadProject")?.addEventListener("click", () => uploadProjectIntoExplorer());
  document.getElementById("sideMoreActions")?.addEventListener("click", () => {
    ideState.menuOpen = !ideState.menuOpen;
    renderSidePanel();
  });
  document.getElementById("startCreateProject")?.addEventListener("click", openCreateProjectDialog);
  document.getElementById("startUploadProject")?.addEventListener("click", () => uploadProjectIntoExplorer());
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
    if (tab) openFileInEditor(tab.dataset.tabFile);
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
    const start = event.target.closest("#terminalStart");
    if (start) startTerminal();
  });

  document.getElementById("toolBody")?.addEventListener("input", (event) => {
    if (event.target.id === "terminalInput") ideState.terminalInput = event.target.value;
  });

  document.getElementById("toolBody")?.addEventListener("submit", async (event) => {
    if (event.target.id !== "terminalForm") return;
    event.preventDefault();
    await sendTerminalCommand(event.target.querySelector("#terminalInput")?.value ?? "");
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
    if (!(event.ctrlKey || event.metaKey)) return;
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

  window.addEventListener("beforeunload", (event) => {
    if (!ideState.dirty) return;
    event.preventDefault();
    event.returnValue = "";
  });
}

async function init() {
  installRendererIcon();
  installStyles();
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
