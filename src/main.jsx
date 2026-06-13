import wapiIconUrl from "../wapi.png";
import bannerIconUrl from "../banner icon.png";
import evaluatorSource from "../evaluator.cpp?raw";
import * as monaco from "monaco-editor/esm/vs/editor/editor.api";
import editorWorker from "monaco-editor/esm/vs/editor/editor.worker?worker";

self.MonacoEnvironment = {
  getWorker: () => new editorWorker()
};

const bridge = window.wapi ?? {
  addFiles: async () => [],
  createProject: async (payload = {}) => ({
    rootPath: null,
    files: payload.files ?? []
  }),
  loadProject: async () => null,
  window: {
    minimize: async () => null,
    toggleMaximize: async () => null,
    close: async () => null
  }
};

const explorerState = {
  projectFiles: [],
  activeFileId: null,
  projectRoot: null,
  searchQuery: ""
};

const editorState = {
  editor: null,
  model: null,
  changeDisposable: null,
  cursorDisposable: null
};

const uiState = {
  activePanel: "explorer",
  terminalCollapsed: false,
  terminalHidden: false,
  menuOpen: false,
  projectDialogOpen: false
};

const welcomeSource = "";

function escapeRegExp(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
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
  const entryPattern = /\{\s*"([a-zA-Z_]\w*)"\s*,\s*\{\s*(\d+)\s*,\s*"([^"]+)"\s*,\s*(true|false)\s*,/g;
  const entries = [...registrySource.matchAll(entryPattern)];

  return entries.map((entry, index) => {
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
}

const wapiRuntimeFunctions = parseEvaluatorFunctionRegistry(evaluatorSource);
const wapiFunctionNameRegex = wapiRuntimeFunctions.length
  ? new RegExp(`\\b(?:${wapiRuntimeFunctions.map((fn) => escapeRegExp(fn.name)).join("|")})\\b`)
  : /(?!)/;

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

function activeExplorerFile() {
  return explorerState.projectFiles.find((file) => file.id === explorerState.activeFileId) ?? null;
}

function languageForFile(file) {
  const path = (file?.name || file?.relativePath || "").toLowerCase();
  if (path.endsWith(".cpp") || path.endsWith(".c") || path.endsWith(".h") || path.endsWith(".hpp")) return "cpp";
  if (path.endsWith(".txt")) return "plaintext";
  return "wapi";
}

function normalizePath(value = "") {
  return String(value).replace(/\\/g, "/");
}

function normalizeProjectFile(file = {}) {
  const filePath = typeof file.filePath === "string" ? file.filePath : "";
  const relativePath = normalizePath(
    typeof file.relativePath === "string" && file.relativePath
      ? file.relativePath
      : filePath.split(/[\\/]/).pop() || file.name || "Untitled"
  );
  const name = file.name || relativePath.split("/").pop() || "Untitled";
  const id = filePath || relativePath || name;
  return { ...file, id, filePath, name, relativePath };
}

function starterProjectFiles(projectName = "WapiProject") {
  const safeName = projectName.trim() || "WapiProject";
  return [
    {
      name: "main.wapi",
      relativePath: "main.wapi",
      source: [
        `// ${safeName}`,
        "listProcesses()",
        ""
      ].join("\n")
    },
    {
      name: "README.txt",
      relativePath: "README.txt",
      source: [
        safeName,
        "",
        "Created with Wapi IDE.",
        "Start in main.wapi."
      ].join("\n")
    }
  ];
}

function mergeProjectFiles(existingFiles, incomingFiles) {
  const filesById = new Map(existingFiles.map((file) => [file.id, file]));
  for (const file of incomingFiles.map(normalizeProjectFile)) {
    filesById.set(file.id, file);
  }
  return [...filesById.values()].sort((a, b) => a.relativePath.localeCompare(b.relativePath));
}

function explorerGroups(files) {
  const groups = new Map();
  for (const file of files) {
    const parts = file.relativePath.split("/");
    const groupName = parts.length > 1 ? parts.slice(0, -1).join("/") : "Loose Files";
    if (!groups.has(groupName)) groups.set(groupName, []);
    groups.get(groupName).push(file);
  }
  return [...groups.entries()].sort(([a], [b]) => a.localeCompare(b));
}

function filteredExplorerFiles() {
  const query = explorerState.searchQuery.trim().toLowerCase();
  if (!query) return explorerState.projectFiles;
  return explorerState.projectFiles.filter((file) => {
    const searchable = [
      file.name,
      file.relativePath,
      file.relativePath.split("/").slice(0, -1).join("/")
    ].join(" ").toLowerCase();
    return searchable.includes(query);
  });
}

function replaceProject(project, message) {
  explorerState.projectFiles = project.files.map(normalizeProjectFile)
    .sort((a, b) => a.relativePath.localeCompare(b.relativePath));
  explorerState.activeFileId = explorerState.projectFiles[0]?.id ?? null;
  explorerState.projectRoot = project.rootPath ?? null;
  explorerState.searchQuery = "";
  uiState.activePanel = "explorer";
  uiState.menuOpen = false;
  renderSolutionExplorer();
  setEditorModel(activeExplorerFile());
  updateStartSurface();
  if (message) setStatusMessage(message);
}

function projectDisplayName() {
  if (!explorerState.projectRoot) return "WapiProject";
  const normalized = normalizePath(explorerState.projectRoot);
  return normalized.split("/").filter(Boolean).pop() || "WapiProject";
}

function setStatusMessage(message) {
  const statusInstance = document.getElementById("statusInstance");
  if (!statusInstance) return;
  statusInstance.textContent = message;
  statusInstance.classList.remove("is-live");
  statusInstance.offsetWidth;
  statusInstance.classList.add("is-live");
  window.clearTimeout(setStatusMessage.timeoutId);
  setStatusMessage.timeoutId = window.setTimeout(() => {
    statusInstance.textContent = "No Instance";
    statusInstance.classList.remove("is-live");
  }, 1800);
}

function updateCursorStatus() {
  const cursor = document.getElementById("statusCursor");
  if (!cursor || !editorState.editor) return;
  const position = editorState.editor.getPosition();
  cursor.textContent = position ? `Ln ${position.lineNumber}, Col ${position.column}` : "Ln 1, Col 1";
}

function setActivePanel(panel) {
  uiState.activePanel = panel;
  uiState.menuOpen = false;
  renderSolutionExplorer();
  document.querySelectorAll(".activity-button[data-panel]").forEach((button) => {
    button.classList.toggle("is-active", button.dataset.panel === panel);
  });
}

function renderPanelEmpty(sectionName, message) {
  const tree = document.getElementById("explorerFileTree");
  if (!tree) return;
  const sectionTitle = document.createElement("div");
  sectionTitle.className = "explorer-section-title";
  sectionTitle.textContent = sectionName;
  const empty = document.createElement("div");
  empty.className = "explorer-empty";
  empty.textContent = message;
  tree.append(sectionTitle, empty);
}

function renderFileGroups(files, emptySection = "FILES", emptyMessage = "No files found") {
  const tree = document.getElementById("explorerFileTree");
  if (!tree) return;
  if (files.length === 0) {
    renderPanelEmpty(emptySection, emptyMessage);
    return;
  }

  for (const [groupName, groupFiles] of explorerGroups(files)) {
    const section = document.createElement("section");
    section.className = "explorer-section";

    const title = document.createElement("div");
    title.className = "explorer-section-title";
    title.textContent = groupName;
    section.appendChild(title);

    for (const file of groupFiles) {
      const item = document.createElement("button");
      item.className = `explorer-file${file.id === explorerState.activeFileId ? " is-active" : ""}`;
      item.type = "button";
      item.dataset.fileId = file.id;
      item.title = file.relativePath;
      item.innerHTML = `
        <svg class="file-document-icon" viewBox="0 0 24 24" aria-hidden="true">
          <path d="M7 3h7l5 5v13H7z"></path>
          <path d="M14 3v5h5"></path>
          <path d="M10 13h6"></path>
          <path d="M10 17h4"></path>
        </svg>
        <span class="explorer-file-name"></span>
      `;
      item.querySelector(".explorer-file-name").textContent = file.name;
      section.appendChild(item);
    }

    tree.appendChild(section);
  }
}

function renderStaticPanel(sectionName, message, actions = []) {
  const tree = document.getElementById("explorerFileTree");
  if (!tree) return;
  renderPanelEmpty(sectionName, message);
  if (actions.length === 0) return;

  const card = document.createElement("div");
  card.className = "panel-card";
  for (const action of actions) {
    const button = document.createElement("button");
    button.className = "panel-button";
    button.type = "button";
    button.dataset.action = action.id;
    button.textContent = action.label;
    card.appendChild(button);
  }
  tree.appendChild(card);
}

function renderSolutionExplorer() {
  const meta = document.getElementById("explorerMeta");
  const title = document.getElementById("explorerTitle");
  const actions = document.getElementById("explorerActions");
  const searchbar = document.getElementById("explorerSearchbar");
  const searchInput = document.getElementById("explorerSearch");
  const tree = document.getElementById("explorerFileTree");
  if (!meta || !tree) return;

  const files = explorerState.projectFiles;
  const visibleFiles = filteredExplorerFiles();
  const panel = uiState.activePanel;

  const panelLabels = {
    explorer: "EXPLORER",
    search: "SEARCH",
    source: "SOURCE CONTROL",
    account: "ACCOUNT",
    settings: "SETTINGS"
  };
  if (title) title.textContent = panelLabels[panel] ?? "EXPLORER";
  actions?.classList.toggle("is-hidden", panel !== "explorer");
  searchbar?.classList.toggle("is-hidden", !["explorer", "search"].includes(panel));
  document.getElementById("explorerActionMenu")?.classList.toggle("is-open", uiState.menuOpen);
  document.getElementById("explorerMoreActions")?.setAttribute("aria-expanded", String(uiState.menuOpen));

  if (searchInput && searchInput.value !== explorerState.searchQuery) {
    searchInput.value = explorerState.searchQuery;
  }
  if (searchInput) {
    searchInput.placeholder = panel === "search" ? "Search workspace" : "Search files";
  }

  meta.textContent = files.length === 0
    ? "No files loaded"
    : explorerState.searchQuery
      ? `${visibleFiles.length} match${visibleFiles.length === 1 ? "" : "es"}`
    : `${files.length} file${files.length === 1 ? "" : "s"}`;

  updateStartSurface();
  tree.replaceChildren();

  if (panel === "search") {
    if (!explorerState.searchQuery.trim()) {
      renderPanelEmpty("QUERY", "Type to search files");
      return;
    }
    renderFileGroups(visibleFiles, "RESULTS", files.length === 0 ? "No files loaded" : "No results found");
    return;
  }

  if (panel === "source") {
    renderStaticPanel("CHANGES", "No source control changes");
    return;
  }

  if (panel === "account") {
    renderStaticPanel("PROFILE", "No account connected");
    return;
  }

  if (panel === "settings") {
    renderStaticPanel("EDITOR", "Editor preferences", [
      { id: "toggle-minimap", label: "Toggle minimap" },
      { id: "toggle-terminal", label: uiState.terminalHidden ? "Show terminal" : "Hide terminal" }
    ]);
    return;
  }

  if (files.length === 0) {
    renderPanelEmpty("START", "Create or upload a project");
    return;
  }

  if (visibleFiles.length === 0) {
    renderPanelEmpty("FILES", "No matching files");
    return;
  }

  renderFileGroups(visibleFiles);
  updateStartSurface();
}

function updateStartSurface() {
  const startSurface = document.getElementById("startSurface");
  const monacoHost = document.getElementById("monacoEditor");
  const editorSurface = document.querySelector(".editor-surface");
  const projectName = document.getElementById("startProjectName");
  const projectPath = document.getElementById("startProjectPath");
  const showStart = explorerState.projectFiles.length === 0;

  startSurface?.classList.toggle("is-visible", showStart);
  monacoHost?.classList.toggle("is-start-hidden", showStart);
  editorSurface?.classList.toggle("is-start-mode", showStart);

  if (projectName) {
    projectName.textContent = showStart ? "No project loaded" : projectDisplayName();
  }
  if (projectPath) {
    projectPath.textContent = showStart
      ? "Create a Wapi project or upload an existing folder."
      : explorerState.projectRoot ?? `${explorerState.projectFiles.length} file${explorerState.projectFiles.length === 1 ? "" : "s"}`;
  }
}

function setProjectDialogOpen(open) {
  uiState.projectDialogOpen = open;
  const dialog = document.getElementById("newProjectDialog");
  const input = document.getElementById("newProjectName");
  dialog?.classList.toggle("is-open", open);
  dialog?.setAttribute("aria-hidden", String(!open));
  if (open && input) {
    input.value = "WapiProject";
    input.focus();
    input.select();
  }
}

async function createProjectFromDialog() {
  const input = document.getElementById("newProjectName");
  const name = input?.value.trim() || "WapiProject";
  const project = await bridge.createProject({
    name,
    files: starterProjectFiles(name)
  });
  if (!project || !Array.isArray(project.files) || project.files.length === 0) {
    setProjectDialogOpen(false);
    setStatusMessage("Project creation cancelled");
    return;
  }
  setProjectDialogOpen(false);
  replaceProject(project, "Project created");
}

function installMonacoLanguage() {
  monaco.languages.register({ id: "wapi" });
  monaco.languages.setMonarchTokensProvider("wapi", {
    tokenizer: {
      root: [
        [/\/\/.*$/, "comment"],
        [/"([^"\\]|\\.)*$/, "string.invalid"],
        [/"/, "string", "@string"],
        [/\b(?:print|let|if|else|while|for|return|true|false|null|check|run)\b/, "keyword"],
        [wapiFunctionNameRegex, "type.identifier"],
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
          documentation: [
            `Capability: ${fn.capability}`,
            fn.requiresInjectionFlag ? "Requires --allow-injection outside unsafe mode." : ""
          ].filter(Boolean).join("\n"),
          insertText: wapiFunctionSnippet(fn),
          insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
          range,
          sortText: `0_${fn.name}`
        }))
      };
    }
  });
  monaco.editor.defineTheme("wapi-dark", {
    base: "vs-dark",
    inherit: true,
    rules: [
      { token: "comment", foreground: "6f6f6f" },
      { token: "keyword", foreground: "ffffff", fontStyle: "bold" },
      { token: "type.identifier", foreground: "cfcfcf" },
      { token: "string", foreground: "d7d7d7" },
      { token: "number", foreground: "b8b8b8" }
    ],
    colors: {
      "editor.background": "#18181c",
      "editor.foreground": "#c6c9d2",
      "editorLineNumber.foreground": "#777d8a",
      "editorLineNumber.activeForeground": "#aeb3c0",
      "editorCursor.foreground": "#d9dde6",
      "editor.selectionBackground": "#334052",
      "editor.inactiveSelectionBackground": "#292d36",
      "editor.lineHighlightBackground": "#202126",
      "editorLineNumber.dimmedForeground": "#5d626e",
      "editorIndentGuide.background1": "#2d3038",
      "editorIndentGuide.activeBackground1": "#484e5c",
      "editorWidget.background": "#1c1d22",
      "editorWidget.border": "#343844",
      "minimap.background": "#18181c",
      "minimapGutter.addedBackground": "#2aa866",
      "minimapGutter.modifiedBackground": "#6f84d9",
      "minimapSlider.background": "#59606f33",
      "minimapSlider.hoverBackground": "#6b73844d",
      "minimapSlider.activeBackground": "#7b849866",
      "input.background": "#25262d",
      "input.border": "#323641"
    }
  });
}

function setEditorModel(file) {
  if (!editorState.editor) return;

  const source = file?.source ?? welcomeSource;
  const language = languageForFile(file);
  const uri = monaco.Uri.parse(`wapi://workspace/${encodeURIComponent(file?.id ?? "welcome.wapi")}`);
  const existing = monaco.editor.getModel(uri);
  const model = existing ?? monaco.editor.createModel(source, language, uri);

  if (existing && existing.getValue() !== source && file) {
    existing.setValue(source);
  }

  editorState.changeDisposable?.dispose();
  editorState.model = model;
  editorState.editor.setModel(model);
  editorState.editor.updateOptions({ readOnly: false });
  const label = document.getElementById("editorTabLabel");
  const labelText = document.getElementById("editorTabText");
  const status = document.getElementById("editorStatus");
  const statusFile = document.getElementById("statusFile");
  const statusLanguage = document.getElementById("statusLanguage");
  if (labelText) {
    labelText.textContent = file?.name ?? "Welcome";
  }
  if (label) {
    label.title = file?.relativePath ?? "Welcome";
    label.classList.toggle("is-file-tab", Boolean(file));
  }
  if (status) {
    status.textContent = language === "cpp" ? "C++" : language === "plaintext" ? "Text" : "Wapi";
  }
  if (statusFile) {
    statusFile.textContent = `</> ${file?.name ?? "Welcome"}`;
    statusFile.title = file?.relativePath ?? "Welcome";
  }
  if (statusLanguage) {
    statusLanguage.textContent = language === "cpp" ? "C++" : language === "plaintext" ? "Text" : "Wapi";
  }
  updateCursorStatus();
  editorState.changeDisposable = model.onDidChangeContent(() => {
    const active = activeExplorerFile();
    if (active && model === editorState.model) active.source = model.getValue();
  });
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
      maxColumn: 88
    },
    scrollBeyondLastLine: false,
    renderLineHighlight: "line",
    roundedSelection: false,
    cursorBlinking: "smooth",
    cursorSmoothCaretAnimation: "on",
    overviewRulerBorder: false,
    hideCursorInOverviewRuler: true,
    fixedOverflowWidgets: true,
    padding: { top: 14, bottom: 14 }
  });
  editorState.cursorDisposable?.dispose();
  editorState.cursorDisposable = editorState.editor.onDidChangeCursorPosition(updateCursorStatus);
  setEditorModel(activeExplorerFile());
}

async function addFilesToExplorer() {
  const files = await bridge.addFiles();
  if (!Array.isArray(files) || files.length === 0) {
    setStatusMessage("No files added");
    return;
  }
  const normalized = files.map(normalizeProjectFile);
  explorerState.projectFiles = mergeProjectFiles(explorerState.projectFiles, normalized);
  explorerState.activeFileId ||= normalized[0].id;
  explorerState.projectRoot = null;
  renderSolutionExplorer();
  setEditorModel(activeExplorerFile());
  updateStartSurface();
  setStatusMessage(`${normalized.length} file${normalized.length === 1 ? "" : "s"} added`);
}

async function uploadProjectIntoExplorer() {
  const project = await bridge.loadProject();
  if (!project || !Array.isArray(project.files)) {
    setStatusMessage("Project upload cancelled");
    return;
  }
  replaceProject(project, "Project uploaded");
}

function refreshExplorerPanel() {
  explorerState.projectFiles = [...explorerState.projectFiles]
    .sort((a, b) => a.relativePath.localeCompare(b.relativePath));
  renderSolutionExplorer();
  setStatusMessage("Explorer refreshed");
}

function toggleExplorerMenu() {
  uiState.menuOpen = !uiState.menuOpen;
  renderSolutionExplorer();
}

function setTerminalState(nextState = {}) {
  Object.assign(uiState, nextState);
  const terminal = document.getElementById("terminalPanel");
  const collapse = document.getElementById("terminalCollapse");
  const statusTerminal = document.getElementById("statusTerminal");
  terminal?.classList.toggle("is-collapsed", uiState.terminalCollapsed);
  terminal?.classList.toggle("is-hidden", uiState.terminalHidden);
  collapse?.setAttribute("aria-pressed", String(uiState.terminalCollapsed));
  collapse?.classList.toggle("is-active", uiState.terminalCollapsed);
  if (statusTerminal) {
    statusTerminal.textContent = uiState.terminalHidden ? "Terminal hidden" : "Terminal";
    statusTerminal.classList.toggle("is-active", !uiState.terminalHidden);
  }
  editorState.editor?.layout();
}

function toggleMinimap() {
  if (!editorState.editor) return;
  const current = editorState.editor.getOption(monaco.editor.EditorOption.minimap).enabled;
  editorState.editor.updateOptions({ minimap: { enabled: !current } });
  setStatusMessage(!current ? "Minimap enabled" : "Minimap disabled");
}

function handlePanelAction(action) {
  if (action === "toggle-minimap") {
    toggleMinimap();
    return;
  }
  if (action === "toggle-terminal") {
    setTerminalState({ terminalHidden: !uiState.terminalHidden, terminalCollapsed: false });
    renderSolutionExplorer();
  }
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
      background: #15161a;
      --window-chrome-height: 38px;
      --status-height: 23px;
      --window-signature-height: 22px;
      --window-signature-width: 82px;
      --window-signature-image-height: 33.2px;
      --window-signature-left: 14px;
      --window-signature-inset: calc((var(--window-chrome-height) - var(--window-signature-height)) / 2);
      --window-control-size: 30px;
      --activity-width: 66px;
      --explorer-width: 228px;
      --accent: #39c987;
      --accent-soft: rgba(57, 201, 135, 0.13);
      --accent-blue: #6679d8;
      --panel: #17181c;
      --panel-raised: #1b1c21;
      --line: rgba(218, 224, 238, 0.055);
      --text-main: #c5c9d3;
      --text-muted: #8f96a5;
      --text-soft: #aab0be;
      --focus-ring: 0 0 0 1px rgba(57, 201, 135, 0.42), 0 0 0 4px rgba(57, 201, 135, 0.1);
      --shadow-soft: 0 14px 36px rgba(0, 0, 0, 0.28);
      --motion-fast: 120ms cubic-bezier(.2, .7, .2, 1);
      --motion-med: 180ms cubic-bezier(.2, .7, .2, 1);
      --motion-slow: 260ms cubic-bezier(.16, 1, .3, 1);
    }

    * {
      box-sizing: border-box;
    }

    html,
    body,
    #root {
      width: 100%;
      height: 100%;
      margin: 0;
      overflow: hidden;
    }

    body {
      color: var(--text-main);
      background: #15161a;
      -webkit-user-select: none;
      user-select: none;
    }

    ::selection {
      color: #f4fff9;
      background: rgba(57, 201, 135, 0.34);
    }

    button,
    input {
      font: inherit;
    }

    button:focus-visible,
    input:focus-visible {
      outline: none;
      box-shadow: var(--focus-ring);
    }

    @keyframes surfaceIn {
      from {
        opacity: 0;
        transform: translateY(6px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    @keyframes menuIn {
      from {
        opacity: 0;
        transform: translateY(-6px) scale(0.98);
      }
      to {
        opacity: 1;
        transform: translateY(0) scale(1);
      }
    }

    @keyframes statusPulse {
      0%,
      100% {
        box-shadow: 0 0 8px rgba(199, 76, 76, 0.3);
      }
      50% {
        box-shadow: 0 0 14px rgba(199, 76, 76, 0.58);
      }
    }

    @keyframes statusFlash {
      0% {
        color: #f4fff9;
        text-shadow: 0 0 10px rgba(57, 201, 135, 0.44);
      }
      100% {
        color: inherit;
        text-shadow: none;
      }
    }

    .app-shell {
      width: 100%;
      height: 100%;
      background: linear-gradient(180deg, #17181d 0%, #141519 100%);
    }

    #windowChrome {
      position: fixed;
      top: 0;
      left: 0;
      right: 0;
      z-index: 10;
      display: flex;
      align-items: center;
      justify-content: space-between;
      height: var(--window-chrome-height);
      min-height: var(--window-chrome-height);
      padding: 0 16px 0 var(--window-signature-left);
      border: 1px solid var(--line);
      border-bottom-color: rgba(255, 255, 255, 0.06);
      background:
        linear-gradient(180deg, rgba(255, 255, 255, 0.018), rgba(0, 0, 0, 0)),
        #17181c;
      box-shadow:
        inset 0 -1px rgba(0, 0, 0, 0.8),
        0 10px 28px rgba(0, 0, 0, 0.14);
      -webkit-app-region: drag;
    }

    #windowTitleGroup {
      display: flex;
      align-items: center;
      min-width: 0;
      max-width: calc(100% - 132px);
      height: 100%;
      pointer-events: none;
    }

    #windowAppTitle {
      position: absolute;
      left: 50%;
      top: 50%;
      overflow: hidden;
      max-width: 220px;
      color: var(--text-soft);
      font-size: 12px;
      font-weight: 600;
      line-height: 1;
      text-overflow: ellipsis;
      white-space: nowrap;
      transform: translate(-50%, -50%);
      pointer-events: none;
    }

    #windowBannerFrame {
      position: relative;
      display: block;
      width: var(--window-signature-width);
      height: var(--window-signature-height);
      overflow: hidden;
      flex: 0 1 auto;
    }

    #windowBannerIcon {
      position: absolute;
      left: -0.2px;
      top: -0.5px;
      display: block;
      width: auto;
      height: var(--window-signature-image-height);
      max-width: none;
    }

    #windowButtons {
      display: flex;
      align-items: center;
      gap: 8px;
      height: 100%;
      -webkit-app-region: no-drag;
    }

    .window-btn {
      position: relative;
      display: flex;
      align-items: center;
      justify-content: center;
      flex: 0 0 var(--window-control-size);
      width: var(--window-control-size);
      height: var(--window-control-size);
      margin: 0;
      padding: 0;
      border: none;
      outline: none;
      border-radius: 4px;
      color: #9da3b1;
      background: transparent;
      cursor: pointer;
      line-height: 1;
      transition:
        color var(--motion-fast),
        background-color var(--motion-fast),
        transform var(--motion-fast);
    }

    .window-btn:hover {
      color: #d9dde6;
      background: rgba(218, 224, 238, 0.055);
      transform: translateY(-1px);
    }

    .window-btn:active {
      transform: translateY(0) scale(0.95);
    }

    .window-btn-close:hover {
      color: #f0f2f7;
      background: #a43b3b;
    }

    .window-icon {
      position: absolute;
      left: 50%;
      top: 50%;
      display: block;
      width: 12px;
      height: 12px;
      transform: translate(-50%, -50%);
      pointer-events: none;
      transition: transform var(--motion-fast);
    }

    .window-btn:hover .window-icon {
      transform: translate(-50%, -50%) scale(1.08);
    }

    .window-icon-min::before {
      position: absolute;
      left: 1px;
      right: 1px;
      top: 50%;
      height: 1.6px;
      border-radius: 2px;
      background: currentColor;
      content: "";
      transform: translateY(-50%);
    }

    .window-icon-max::before {
      position: absolute;
      inset: 1px;
      border: 1.8px solid currentColor;
      border-radius: 1.5px;
      content: "";
    }

    .window-icon-close::before,
    .window-icon-close::after {
      position: absolute;
      left: 0;
      top: 50%;
      width: 12px;
      height: 1.4px;
      border-radius: 1px;
      background: currentColor;
      content: "";
      transform-origin: center;
    }

    .window-icon-close::before {
      transform: rotate(45deg);
    }

    .window-icon-close::after {
      transform: rotate(-45deg);
    }

    .blank-stage {
      display: flex;
      gap: 0;
      width: 100%;
      height: 100%;
      padding-top: var(--window-chrome-height);
      padding-bottom: var(--status-height);
      background:
        linear-gradient(90deg, rgba(255, 255, 255, 0.012), transparent 34%),
        #15161a;
      min-height: 0;
      animation: surfaceIn 240ms ease-out both;
    }

    .activity-bar {
      display: flex;
      flex: 0 0 var(--activity-width);
      flex-direction: column;
      align-items: stretch;
      height: 100%;
      padding: 14px 0 12px;
      border-right: 1px solid var(--line);
      background:
        linear-gradient(180deg, rgba(255, 255, 255, 0.018), rgba(0, 0, 0, 0.06)),
        #17181c;
    }

    .activity-button {
      position: relative;
      display: flex;
      align-items: center;
      justify-content: center;
      flex: 0 0 46px;
      width: 46px;
      height: 46px;
      margin: 0;
      margin-inline: auto;
      padding: 0;
      border: 0;
      border-radius: 9px;
      color: #8d94a4;
      background: transparent;
      cursor: pointer;
      -webkit-app-region: no-drag;
      transition:
        color var(--motion-fast),
        background-color var(--motion-fast),
        box-shadow var(--motion-med),
        transform var(--motion-fast);
    }

    .activity-button::before {
      position: absolute;
      left: -10px;
      top: 50%;
      width: 3px;
      height: 18px;
      border-radius: 999px;
      background: var(--accent);
      content: "";
      opacity: 0;
      transform: translateY(-50%) scaleY(0.45);
      transition:
        opacity var(--motion-fast),
        transform var(--motion-med);
    }

    .activity-button:hover,
    .activity-button.is-active {
      color: #e1e5ed;
    }

    .activity-button:hover {
      background: rgba(218, 224, 238, 0.052);
      transform: translateY(-1px);
    }

    .activity-button.is-active {
      background:
        linear-gradient(135deg, rgba(57, 201, 135, 0.88) 0%, rgba(75, 155, 205, 0.86) 100%);
      box-shadow:
        0 8px 22px rgba(57, 201, 135, 0.14),
        inset 0 1px rgba(255, 255, 255, 0.14);
    }

    .activity-button.is-active::before {
      opacity: 1;
      transform: translateY(-50%) scaleY(1);
    }

    .activity-button:active {
      transform: translateY(0) scale(0.96);
    }

    .activity-spacer {
      flex: 1 1 auto;
    }

    .ui-icon {
      display: block;
      flex: 0 0 auto;
      width: 16px;
      height: 16px;
      fill: none;
      stroke: currentColor;
      stroke-width: 1.75;
      stroke-linecap: round;
      stroke-linejoin: round;
      vector-effect: non-scaling-stroke;
    }

    .activity-button .ui-icon {
      width: 24px;
      height: 24px;
      stroke-width: 1.55;
      transition: transform var(--motion-med), stroke-width var(--motion-fast);
    }

    .activity-button:hover .ui-icon,
    .activity-button.is-active .ui-icon {
      transform: scale(1.04);
    }

    #solutionExplorer {
      position: relative;
      display: flex;
      flex: 0 0 var(--explorer-width);
      flex-direction: column;
      min-width: 0;
      height: 100%;
      margin: 0;
      border: 0;
      border-right: 1px solid var(--line);
      background:
        linear-gradient(180deg, rgba(255, 255, 255, 0.014), rgba(0, 0, 0, 0.018)),
        var(--panel);
      box-shadow: none;
      min-height: 0;
    }

    #solutionExplorer::after {
      position: absolute;
      top: 0;
      right: 0;
      bottom: 0;
      width: 1px;
      background: var(--line);
      content: "";
      pointer-events: none;
    }

    .explorer-topbar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex: 0 0 58px;
      min-width: 0;
      padding: 0 14px;
      border-bottom: 0;
      background:
        linear-gradient(180deg, rgba(255, 255, 255, 0.012), transparent),
        var(--panel);
    }

    .explorer-title-block {
      min-width: 0;
    }

    .explorer-title {
      overflow: hidden;
      color: var(--text-main);
      font-size: 10px;
      font-weight: 700;
      letter-spacing: 1.2px;
      line-height: 1.1;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .explorer-meta {
      display: none;
    }

    .explorer-actions {
      display: flex;
      align-items: center;
      gap: 8px;
      flex: 0 0 auto;
      margin-left: 10px;
    }

    .explorer-actions.is-hidden,
    .explorer-searchbar.is-hidden {
      display: none;
    }

    .explorer-action {
      position: relative;
      display: flex;
      align-items: center;
      justify-content: center;
      width: 22px;
      height: 22px;
      margin: 0;
      padding: 0;
      border: 0;
      border-radius: 5px;
      color: #969dac;
      background: transparent;
      cursor: pointer;
      -webkit-app-region: no-drag;
      transition:
        border-color var(--motion-fast),
        color var(--motion-fast),
        background-color var(--motion-fast),
        box-shadow var(--motion-fast),
        transform var(--motion-fast);
    }

    .explorer-action:hover,
    .explorer-action[aria-expanded="true"] {
      color: #dde1ea;
      background: rgba(218, 224, 238, 0.06);
      transform: translateY(-1px);
    }

    .explorer-action:active {
      transform: translateY(0) scale(0.94);
    }

    .explorer-action .ui-icon {
      width: 16px;
      height: 16px;
      stroke-width: 1.75;
      transition: transform var(--motion-fast);
    }

    .explorer-action:hover .ui-icon,
    .explorer-action[aria-expanded="true"] .ui-icon {
      transform: scale(1.08);
    }

    .explorer-action-menu {
      position: absolute;
      top: 48px;
      right: 9px;
      z-index: 4;
      display: none;
      min-width: 142px;
      padding: 5px;
      border: 1px solid var(--line);
      border-radius: 7px;
      background: #202129;
      box-shadow: var(--shadow-soft);
      transform-origin: 92% 0;
    }

    .explorer-action-menu.is-open {
      display: grid;
      gap: 2px;
      animation: menuIn var(--motion-slow) both;
    }

    .menu-item,
    .panel-button {
      width: 100%;
      min-height: 26px;
      padding: 0 9px;
      border: 0;
      border-radius: 4px;
      color: #c2c6d0;
      background: transparent;
      cursor: pointer;
      font: inherit;
      font-size: 12px;
      text-align: left;
      -webkit-app-region: no-drag;
      transition:
        color var(--motion-fast),
        background-color var(--motion-fast),
        padding-left var(--motion-fast),
        transform var(--motion-fast);
    }

    .menu-item:hover,
    .panel-button:hover {
      color: #dde1ea;
      background: rgba(218, 224, 238, 0.06);
      padding-left: 12px;
    }

    .menu-item:active,
    .panel-button:active {
      transform: scale(0.985);
    }

    .explorer-searchbar {
      display: flex;
      align-items: center;
      flex: 0 0 58px;
      padding: 8px 16px 12px 14px;
      border-bottom: 0;
      background: var(--panel);
    }

    .explorer-search-wrap {
      position: relative;
      width: 100%;
      height: 32px;
      transition: transform var(--motion-fast);
    }

    .explorer-search-wrap:focus-within {
      transform: translateY(-1px);
    }

    .explorer-search-icon {
      position: absolute;
      left: 12px;
      top: 50%;
      width: 14px;
      height: 14px;
      color: #838b9c;
      transform: translateY(-50%);
      pointer-events: none;
      transition:
        color var(--motion-fast),
        transform var(--motion-fast);
    }

    .explorer-search-wrap:focus-within .explorer-search-icon {
      color: var(--accent);
      transform: translateY(-50%) scale(1.08);
    }

    #explorerSearch {
      width: 100%;
      height: 100%;
      padding: 0 12px 0 36px;
      border: 1px solid rgba(255, 255, 255, 0.055);
      border-radius: 6px;
      outline: none;
      color: #c7cbd5;
      background: #202129;
      font: inherit;
      font-size: 12px;
      line-height: 32px;
      -webkit-app-region: no-drag;
      transition:
        border-color var(--motion-fast),
        background-color var(--motion-fast),
        box-shadow var(--motion-fast);
    }

    #explorerSearch::placeholder {
      color: #858c9b;
    }

    #explorerSearch:focus {
      border-color: rgba(57, 201, 135, 0.38);
      background: #22232b;
      box-shadow:
        0 0 0 1px rgba(57, 201, 135, 0.14),
        0 8px 20px rgba(0, 0, 0, 0.18);
    }

    .explorer-tree {
      flex: 1 1 auto;
      min-height: 0;
      overflow: auto;
      padding: 2px 14px 14px;
      scrollbar-width: thin;
      scrollbar-color: rgba(126, 133, 148, 0.52) transparent;
    }

    .explorer-tree::-webkit-scrollbar {
      width: 8px;
    }

    .explorer-tree::-webkit-scrollbar-thumb {
      border: 2px solid transparent;
      border-radius: 999px;
      background: rgba(126, 133, 148, 0.52);
      background-clip: padding-box;
    }

    .explorer-empty {
      position: relative;
      margin: 10px 0;
      padding: 0 8px;
      border: 0;
      color: var(--text-muted);
      font-size: 12px;
      line-height: 1.35;
      animation: surfaceIn var(--motion-slow) both;
    }

    .panel-card {
      display: grid;
      gap: 6px;
      margin-top: 12px;
      animation: surfaceIn var(--motion-slow) both;
    }

    .explorer-section {
      animation: surfaceIn var(--motion-slow) both;
    }

    .explorer-section + .explorer-section {
      margin-top: 12px;
    }

    .explorer-section-title {
      margin: 8px 0 12px;
      overflow: hidden;
      color: var(--text-soft);
      font-size: 10px;
      font-weight: 650;
      line-height: 1.2;
      letter-spacing: 1px;
      text-overflow: ellipsis;
      text-transform: uppercase;
      white-space: nowrap;
    }

    .explorer-file {
      position: relative;
      display: flex;
      align-items: center;
      gap: 8px;
      width: 100%;
      min-height: 28px;
      margin: 0;
      padding: 0 9px;
      overflow: hidden;
      border: 1px solid transparent;
      border-radius: 5px;
      color: #c2c6d0;
      background: transparent;
      cursor: pointer;
      font: inherit;
      font-size: 12px;
      line-height: 26px;
      text-align: left;
      -webkit-app-region: no-drag;
      box-shadow: inset 0 0 0 0 rgba(57, 201, 135, 0);
      transition:
        border-color var(--motion-fast),
        color var(--motion-fast),
        background-color var(--motion-fast),
        box-shadow var(--motion-fast),
        transform var(--motion-fast);
    }

    .file-document-icon {
      display: block;
      flex: 0 0 auto;
      width: 15px;
      height: 15px;
      color: #a4abb9;
      fill: none;
      stroke: currentColor;
      stroke-width: 1.55;
      stroke-linecap: round;
      stroke-linejoin: round;
      transition:
        color var(--motion-fast),
        transform var(--motion-fast);
    }

    .explorer-file-name {
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .explorer-file:hover .file-document-icon,
    .explorer-file.is-active .file-document-icon {
      color: #c8ced9;
    }

    .explorer-file::before {
      display: none;
      position: absolute;
      left: 11px;
      top: 8px;
      width: 10px;
      height: 12px;
      border: 1px solid rgba(218, 224, 238, 0.14);
      border-radius: 2px;
      content: "";
    }

    .explorer-file::after {
      display: none;
      position: absolute;
      left: 13px;
      top: 12px;
      width: 6px;
      height: 1px;
      border-radius: 1px;
      background: rgba(218, 224, 238, 0.12);
      content: "";
    }

    .explorer-file:hover {
      color: #dde1ea;
      background: rgba(218, 224, 238, 0.052);
      transform: translateX(2px);
    }

    .explorer-file:hover .file-document-icon {
      transform: translateY(-1px);
    }

    .explorer-file.is-active {
      border-color: transparent;
      color: #e2e6ee;
      background: var(--accent-soft);
      box-shadow: inset 2px 0 0 var(--accent);
    }

    .explorer-file:active {
      transform: translateX(1px) scale(0.99);
    }

    .editor-surface {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      min-width: 0;
      min-height: 0;
      background:
        linear-gradient(180deg, rgba(255, 255, 255, 0.014), rgba(0, 0, 0, 0.018)),
        #18181c;
    }

    .editor-tabs {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex: 0 0 39px;
      min-width: 0;
      border-bottom: 1px solid var(--line);
      background:
        linear-gradient(180deg, rgba(255, 255, 255, 0.014), transparent),
        #17181c;
    }

    .editor-tab {
      position: relative;
      display: flex;
      align-items: center;
      gap: 8px;
      max-width: min(340px, 48vw);
      height: 100%;
      padding: 0 14px;
      overflow: hidden;
      border-right: 1px solid var(--line);
      color: #dfe3eb;
      font-size: 12px;
      line-height: 38px;
      text-overflow: ellipsis;
      white-space: nowrap;
      background: #18181c;
      transition:
        color var(--motion-fast),
        background-color var(--motion-fast);
    }

    .editor-tab::after {
      position: absolute;
      left: 0;
      right: 0;
      bottom: 0;
      height: 1px;
      background: linear-gradient(90deg, var(--accent), rgba(102, 121, 216, 0.78));
      content: "";
      opacity: 0.8;
    }

    .editor-tab-icon {
      display: block;
      flex: 0 0 auto;
      width: 15px;
      height: 15px;
      fill: none;
      stroke: currentColor;
      stroke-width: 1.65;
      stroke-linecap: round;
      stroke-linejoin: round;
    }

    .tab-icon-globe {
      color: var(--accent);
    }

    .tab-icon-document {
      display: none;
      color: #a4abb9;
    }

    .editor-tab.is-file-tab .tab-icon-globe {
      display: none;
    }

    .editor-tab.is-file-tab .tab-icon-document {
      display: block;
    }

    .editor-tab-text {
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .editor-status {
      flex: 0 0 auto;
      padding: 0 14px;
      color: #8f96a5;
      font-size: 11px;
      line-height: 38px;
    }

    .editor-body {
      display: flex;
      flex: 1 1 auto;
      flex-direction: column;
      min-width: 0;
      min-height: 0;
    }

    .editor-workbench {
      position: relative;
      display: flex;
      flex: 1 1 auto;
      min-width: 0;
      min-height: 0;
      overflow: hidden;
      background:
        linear-gradient(135deg, rgba(57, 201, 135, 0.035), transparent 34%),
        linear-gradient(180deg, rgba(255, 255, 255, 0.012), rgba(0, 0, 0, 0.02)),
        #18181c;
    }

    #monacoEditor {
      position: absolute;
      inset: 0;
      flex: 1 1 auto;
      min-width: 0;
      min-height: 0;
      opacity: 1;
      transition: opacity var(--motion-med);
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
      padding: 38px;
      overflow: auto;
      color: #dfe3eb;
      background:
        linear-gradient(135deg, rgba(24, 24, 28, 0.96), rgba(20, 21, 25, 0.98)),
        #18181c;
    }

    .start-surface.is-visible {
      display: flex;
      animation: surfaceIn var(--motion-slow) both;
    }

    .start-shell {
      display: grid;
      grid-template-columns: minmax(240px, 0.82fr) minmax(320px, 1fr);
      gap: 44px;
      width: min(860px, 100%);
      align-items: center;
    }

    .start-identity {
      min-width: 0;
    }

    .start-mark {
      display: grid;
      width: 48px;
      height: 48px;
      margin-bottom: 20px;
      place-items: center;
      border: 1px solid rgba(57, 201, 135, 0.38);
      border-radius: 10px;
      color: #0f1714;
      background: linear-gradient(135deg, #39c987, #89a9ff);
      box-shadow: 0 18px 46px rgba(57, 201, 135, 0.18);
      font-size: 20px;
      font-weight: 800;
    }

    .start-title {
      margin: 0;
      color: #f1f4f8;
      font-size: 34px;
      font-weight: 680;
      line-height: 1.08;
      letter-spacing: 0;
    }

    .start-subtitle {
      max-width: 330px;
      margin: 14px 0 0;
      color: #9ca4b4;
      font-size: 13px;
      line-height: 1.6;
    }

    .start-project-meta {
      display: grid;
      gap: 4px;
      margin-top: 32px;
      color: #8f96a5;
      font-size: 12px;
    }

    .start-project-meta strong {
      color: #c8ced9;
      font-size: 13px;
      font-weight: 600;
    }

    .start-project-meta span {
      overflow: hidden;
      max-width: 360px;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .start-actions-panel {
      display: grid;
      gap: 10px;
      min-width: 0;
    }

    .start-action {
      position: relative;
      display: grid;
      grid-template-columns: 42px 1fr auto;
      gap: 14px;
      align-items: center;
      width: 100%;
      min-height: 76px;
      padding: 14px 16px;
      border: 1px solid rgba(218, 224, 238, 0.08);
      border-radius: 8px;
      color: #dfe3eb;
      background:
        linear-gradient(180deg, rgba(255, 255, 255, 0.026), rgba(255, 255, 255, 0.006)),
        #1d1e24;
      cursor: pointer;
      text-align: left;
      transition:
        border-color var(--motion-fast),
        background-color var(--motion-fast),
        box-shadow var(--motion-med),
        transform var(--motion-fast);
    }

    .start-action:hover {
      border-color: rgba(57, 201, 135, 0.32);
      background-color: #22242b;
      box-shadow: 0 18px 42px rgba(0, 0, 0, 0.23);
      transform: translateY(-2px);
    }

    .start-action:active {
      transform: translateY(0) scale(0.99);
    }

    .start-action-icon {
      display: grid;
      width: 42px;
      height: 42px;
      place-items: center;
      border-radius: 7px;
      color: var(--accent);
      background: rgba(57, 201, 135, 0.11);
    }

    .start-action-primary .start-action-icon {
      color: #121915;
      background: linear-gradient(135deg, #39c987, #89a9ff);
    }

    .start-action-copy {
      min-width: 0;
    }

    .start-action-title {
      display: block;
      color: #f0f3f8;
      font-size: 14px;
      font-weight: 650;
      line-height: 1.2;
    }

    .start-action-note {
      display: block;
      margin-top: 5px;
      overflow: hidden;
      color: #9da5b5;
      font-size: 12px;
      line-height: 1.4;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .start-action-arrow {
      color: #838b9c;
      transition: transform var(--motion-fast), color var(--motion-fast);
    }

    .start-action:hover .start-action-arrow {
      color: #dfe3eb;
      transform: translateX(3px);
    }

    .project-dialog {
      position: fixed;
      inset: 0;
      z-index: 30;
      display: none;
      align-items: center;
      justify-content: center;
      padding: 26px;
      background: rgba(6, 7, 10, 0.58);
      backdrop-filter: blur(14px);
    }

    .project-dialog.is-open {
      display: flex;
      animation: surfaceIn var(--motion-slow) both;
    }

    .project-dialog-card {
      width: min(460px, 100%);
      padding: 20px;
      border: 1px solid rgba(218, 224, 238, 0.09);
      border-radius: 9px;
      background: #1d1e24;
      box-shadow: var(--shadow-soft);
    }

    .project-dialog-card h2 {
      margin: 0 0 16px;
      color: #f0f3f8;
      font-size: 18px;
      font-weight: 680;
      letter-spacing: 0;
    }

    .project-field {
      display: grid;
      gap: 8px;
      color: #aeb5c2;
      font-size: 12px;
      font-weight: 600;
    }

    #newProjectName {
      height: 36px;
      padding: 0 11px;
      border: 1px solid rgba(255, 255, 255, 0.08);
      border-radius: 6px;
      color: #e4e8f0;
      background: #24262e;
      font-size: 13px;
      transition:
        border-color var(--motion-fast),
        box-shadow var(--motion-fast),
        background-color var(--motion-fast);
    }

    #newProjectName:focus {
      border-color: rgba(57, 201, 135, 0.42);
      background: #282a33;
    }

    .project-dialog-actions {
      display: flex;
      justify-content: flex-end;
      gap: 9px;
      margin-top: 20px;
    }

    .dialog-button {
      min-width: 86px;
      height: 32px;
      padding: 0 13px;
      border: 1px solid rgba(218, 224, 238, 0.09);
      border-radius: 6px;
      color: #cbd1dd;
      background: #24262d;
      cursor: pointer;
      font-size: 12px;
      transition:
        border-color var(--motion-fast),
        background-color var(--motion-fast),
        transform var(--motion-fast);
    }

    .dialog-button:hover {
      border-color: rgba(218, 224, 238, 0.18);
      background: #2a2d35;
      transform: translateY(-1px);
    }

    .dialog-button-primary {
      border-color: rgba(57, 201, 135, 0.42);
      color: #101713;
      background: linear-gradient(135deg, #39c987, #89a9ff);
      font-weight: 700;
    }

    @media (max-width: 760px) {
      .start-surface {
        align-items: flex-start;
        padding: 28px 20px;
      }

      .start-shell {
        grid-template-columns: 1fr;
        gap: 26px;
      }

      .start-title {
        font-size: 28px;
      }

      .start-action {
        grid-template-columns: 38px 1fr;
      }

      .start-action-arrow {
        display: none;
      }
    }

    .terminal-panel {
      display: flex;
      flex: 0 0 172px;
      flex-direction: column;
      min-height: 120px;
      border-top: 1px solid var(--line);
      background:
        linear-gradient(180deg, rgba(255, 255, 255, 0.012), rgba(0, 0, 0, 0.018)),
        #18181c;
      transition:
        flex-basis var(--motion-med),
        min-height var(--motion-med),
        opacity var(--motion-med);
    }

    .editor-surface.is-start-mode .terminal-panel {
      display: none;
    }

    .terminal-panel.is-collapsed {
      flex-basis: 34px;
      min-height: 34px;
    }

    .terminal-panel.is-hidden {
      display: none;
    }

    .terminal-panel.is-collapsed .terminal-empty {
      display: none;
    }

    .terminal-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex: 0 0 34px;
      padding: 0 16px;
      border-bottom: 1px solid rgba(255, 255, 255, 0.045);
      color: var(--text-soft);
      font-size: 10px;
      font-weight: 700;
      letter-spacing: 1px;
    }

    .terminal-actions {
      display: flex;
      align-items: center;
      gap: 12px;
      color: #7f8797;
    }

    .terminal-action {
      display: grid;
      width: 18px;
      height: 18px;
      place-items: center;
      border: 0;
      border-radius: 4px;
      color: inherit;
      background: transparent;
      cursor: pointer;
      -webkit-app-region: no-drag;
      rotate: 0deg;
      transition:
        color var(--motion-fast),
        background-color var(--motion-fast),
        transform var(--motion-fast),
        rotate var(--motion-med);
    }

    .terminal-action:hover {
      color: #dde1ea;
      background: rgba(218, 224, 238, 0.055);
      transform: translateY(-1px);
    }

    .terminal-action.is-active {
      color: #c6cbd8;
      background: rgba(218, 224, 238, 0.045);
    }

    #terminalCollapse.is-active {
      rotate: 180deg;
    }

    .terminal-action:active {
      transform: translateY(0) scale(0.94);
    }

    #terminalCollapse .ui-icon {
      transform-box: fill-box;
      transform-origin: center;
      transition: transform var(--motion-med);
    }

    #terminalCollapse.is-active .ui-icon {
      transform: rotate(180deg);
    }

    .terminal-empty {
      display: grid;
      flex: 1 1 auto;
      place-items: center;
      color: #858d9d;
      font-family: "Cascadia Code", "SF Mono", Consolas, monospace;
      text-align: center;
      animation: surfaceIn var(--motion-slow) both;
    }

    .terminal-empty strong {
      display: block;
      margin-bottom: 10px;
      color: var(--text-soft);
      font-size: 12px;
      font-weight: 500;
    }

    .terminal-empty span {
      display: block;
      font-size: 11px;
      opacity: 0.68;
    }

    .status-bar {
      position: fixed;
      left: 0;
      right: 0;
      bottom: 0;
      z-index: 11;
      display: flex;
      align-items: center;
      justify-content: space-between;
      height: var(--status-height);
      padding: 0 9px;
      border: 1px solid var(--line);
      border-top-color: rgba(255, 255, 255, 0.055);
      color: #9fa6b5;
      background:
        linear-gradient(180deg, rgba(255, 255, 255, 0.014), transparent),
        #17181c;
      font-size: 11px;
      line-height: var(--status-height);
    }

    .status-left,
    .status-right {
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
      display: inline-flex;
      align-items: center;
      height: 100%;
      padding: 0;
      border: 0;
      color: inherit;
      background: transparent;
      cursor: pointer;
      font: inherit;
      -webkit-app-region: no-drag;
      transition:
        color var(--motion-fast),
        transform var(--motion-fast);
    }

    .status-button:hover,
    .status-button.is-active {
      color: #cbd0db;
    }

    .status-button:hover {
      transform: translateY(-1px);
    }

    .status-button:active {
      transform: translateY(0) scale(0.98);
    }

    .status-item.is-live {
      animation: statusFlash 900ms ease-out both;
    }

    .status-dot {
      width: 7px;
      height: 7px;
      border-radius: 999px;
      background: #c74c4c;
      box-shadow: 0 0 8px rgba(199, 76, 76, 0.3);
      animation: statusPulse 2.4s ease-in-out infinite;
    }

    @media (prefers-reduced-motion: reduce) {
      *,
      *::before,
      *::after {
        animation-duration: 1ms !important;
        animation-iteration-count: 1 !important;
        scroll-behavior: auto !important;
        transition-duration: 1ms !important;
      }
    }
  `;
  document.head.appendChild(style);
}

function renderWindowBar() {
  const root = document.getElementById("root");
  root.innerHTML = `
    <main class="app-shell">
      <header id="windowChrome">
        <div id="windowTitleGroup">
          <div id="windowBannerFrame">
            <img id="windowBannerIcon" src="${bannerIconUrl}" alt="Wapi">
          </div>
        </div>
        <div id="windowAppTitle">Wapi</div>
        <div id="windowButtons">
          <button id="windowMinimize" class="window-btn window-btn-min" type="button" aria-label="Minimize">
            <span class="window-icon window-icon-min"></span>
          </button>
          <button id="windowMaximize" class="window-btn window-btn-max" type="button" aria-label="Maximize">
            <span class="window-icon window-icon-max"></span>
          </button>
          <button id="windowClose" class="window-btn window-btn-close" type="button" aria-label="Close">
            <span class="window-icon window-icon-close"></span>
          </button>
        </div>
      </header>
      <section class="blank-stage" aria-label="Workspace">
        <nav class="activity-bar" aria-label="Primary navigation">
          <button class="activity-button is-active" type="button" aria-label="Explorer" title="Explorer" data-panel="explorer">
            <svg class="ui-icon" viewBox="0 0 24 24" aria-hidden="true">
              <path d="M8 3h8l4 4v13H8z"></path>
              <path d="M4 7h8l4 4v10H4z"></path>
            </svg>
          </button>
          <button class="activity-button" type="button" aria-label="Search" title="Search" data-panel="search">
            <svg class="ui-icon" viewBox="0 0 24 24" aria-hidden="true">
              <circle cx="11" cy="11" r="6"></circle>
              <path d="m16 16 5 5"></path>
            </svg>
          </button>
          <button class="activity-button" type="button" aria-label="Source control" title="Source control" data-panel="source">
            <svg class="ui-icon" viewBox="0 0 24 24" aria-hidden="true">
              <circle cx="6" cy="5" r="2"></circle>
              <circle cx="18" cy="5" r="2"></circle>
              <circle cx="6" cy="19" r="2"></circle>
              <path d="M6 7v10"></path>
              <path d="M18 7v3a4 4 0 0 1-4 4H6"></path>
            </svg>
          </button>
          <div class="activity-spacer" aria-hidden="true"></div>
          <button class="activity-button" type="button" aria-label="Account" title="Account" data-panel="account">
            <svg class="ui-icon" viewBox="0 0 24 24" aria-hidden="true">
              <circle cx="12" cy="8" r="4"></circle>
              <path d="M5 21a7 7 0 0 1 14 0"></path>
            </svg>
          </button>
          <button class="activity-button" type="button" aria-label="Settings" title="Settings" data-panel="settings">
            <svg class="ui-icon" viewBox="0 0 24 24" aria-hidden="true">
              <path d="M12 15.5a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7Z"></path>
              <path d="M19.4 15a1.8 1.8 0 0 0 .36 1.98l.06.06a2.15 2.15 0 0 1-3.04 3.04l-.06-.06a1.8 1.8 0 0 0-1.98-.36 1.8 1.8 0 0 0-1.08 1.65V21.4a2.15 2.15 0 0 1-4.3 0v-.09a1.8 1.8 0 0 0-1.08-1.65 1.8 1.8 0 0 0-1.98.36l-.06.06a2.15 2.15 0 1 1-3.04-3.04l.06-.06A1.8 1.8 0 0 0 3.6 15a1.8 1.8 0 0 0-1.65-1.08H1.86a2.15 2.15 0 0 1 0-4.3h.09A1.8 1.8 0 0 0 3.6 8.54a1.8 1.8 0 0 0-.36-1.98l-.06-.06A2.15 2.15 0 1 1 6.22 3.46l.06.06a1.8 1.8 0 0 0 1.98.36 1.8 1.8 0 0 0 1.08-1.65V2.14a2.15 2.15 0 0 1 4.3 0v.09a1.8 1.8 0 0 0 1.08 1.65 1.8 1.8 0 0 0 1.98-.36l.06-.06a2.15 2.15 0 1 1 3.04 3.04l-.06.06a1.8 1.8 0 0 0-.36 1.98 1.8 1.8 0 0 0 1.65 1.08h.09a2.15 2.15 0 0 1 0 4.3h-.09A1.8 1.8 0 0 0 19.4 15Z"></path>
            </svg>
          </button>
        </nav>
        <aside id="solutionExplorer" aria-label="Solution Explorer">
          <div class="explorer-topbar">
            <div class="explorer-title-block">
              <div id="explorerTitle" class="explorer-title">EXPLORER</div>
              <div id="explorerMeta" class="explorer-meta">No files loaded</div>
            </div>
            <div id="explorerActions" class="explorer-actions">
              <button id="explorerCreateProject" class="explorer-action" type="button" aria-label="Create new project" title="Create new project">
                <svg class="ui-icon" viewBox="0 0 24 24" aria-hidden="true">
                  <path d="M12 5v14"></path>
                  <path d="M5 12h14"></path>
                </svg>
              </button>
              <button id="explorerUploadProject" class="explorer-action" type="button" aria-label="Upload project" title="Upload project">
                <svg class="ui-icon" viewBox="0 0 24 24" aria-hidden="true">
                  <path d="M3 6.5h6l2 2h10v9.5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"></path>
                  <path d="M3 10h18"></path>
                </svg>
              </button>
              <button id="explorerRefresh" class="explorer-action" type="button" aria-label="Refresh explorer" title="Refresh explorer">
                <svg class="ui-icon" viewBox="0 0 24 24" aria-hidden="true">
                  <path d="M20 11a8 8 0 0 0-14.5-4.5L3 9"></path>
                  <path d="M3 4v5h5"></path>
                  <path d="M4 13a8 8 0 0 0 14.5 4.5L21 15"></path>
                  <path d="M21 20v-5h-5"></path>
                </svg>
              </button>
              <button id="explorerMoreActions" class="explorer-action" type="button" aria-label="More explorer actions" title="More actions" aria-expanded="false">
                <svg class="ui-icon" viewBox="0 0 24 24" aria-hidden="true">
                  <path d="M5 12h.01"></path>
                  <path d="M12 12h.01"></path>
                  <path d="M19 12h.01"></path>
                </svg>
              </button>
            </div>
            <div id="explorerActionMenu" class="explorer-action-menu" role="menu">
              <button class="menu-item" type="button" role="menuitem" data-menu-action="create-project">Create new project</button>
              <button class="menu-item" type="button" role="menuitem" data-menu-action="upload-project">Upload project</button>
              <button class="menu-item" type="button" role="menuitem" data-menu-action="add-files">Add files</button>
              <button class="menu-item" type="button" role="menuitem" data-menu-action="clear-search">Clear search</button>
            </div>
          </div>
          <div id="explorerSearchbar" class="explorer-searchbar">
            <div class="explorer-search-wrap">
              <svg class="explorer-search-icon ui-icon" viewBox="0 0 24 24" aria-hidden="true">
                <circle cx="11" cy="11" r="6"></circle>
                <path d="m16 16 5 5"></path>
              </svg>
              <input id="explorerSearch" type="search" autocomplete="off" spellcheck="false" placeholder="Search files">
            </div>
          </div>
          <div id="explorerFileTree" class="explorer-tree"></div>
        </aside>
        <section class="editor-surface" aria-label="Editor workspace">
          <div class="editor-tabs">
            <div id="editorTabLabel" class="editor-tab">
              <svg class="editor-tab-icon tab-icon-globe" viewBox="0 0 24 24" aria-hidden="true">
                <circle cx="12" cy="12" r="9"></circle>
                <path d="M3 12h18"></path>
                <path d="M12 3a13.8 13.8 0 0 1 0 18"></path>
                <path d="M12 3a13.8 13.8 0 0 0 0 18"></path>
              </svg>
              <svg class="editor-tab-icon tab-icon-document" viewBox="0 0 24 24" aria-hidden="true">
                <path d="M7 3h7l5 5v13H7z"></path>
                <path d="M14 3v5h5"></path>
                <path d="M10 13h6"></path>
                <path d="M10 17h4"></path>
              </svg>
              <span id="editorTabText" class="editor-tab-text">Welcome</span>
            </div>
            <div id="editorStatus" class="editor-status">Wapi</div>
          </div>
          <div class="editor-body">
            <div class="editor-workbench">
              <section id="startSurface" class="start-surface" aria-label="Start">
                <div class="start-shell">
                  <div class="start-identity">
                    <div class="start-mark" aria-hidden="true">W</div>
                    <h1 class="start-title">Wapi</h1>
                    <p class="start-subtitle">Create a Wapi project or upload an existing folder.</p>
                    <div class="start-project-meta">
                      <strong id="startProjectName">No project loaded</strong>
                      <span id="startProjectPath">Create a Wapi project or upload an existing folder.</span>
                    </div>
                  </div>
                  <div class="start-actions-panel">
                    <button id="startCreateProject" class="start-action start-action-primary" type="button">
                      <span class="start-action-icon" aria-hidden="true">
                        <svg class="ui-icon" viewBox="0 0 24 24">
                          <path d="M12 5v14"></path>
                          <path d="M5 12h14"></path>
                        </svg>
                      </span>
                      <span class="start-action-copy">
                        <span class="start-action-title">Create new project</span>
                        <span class="start-action-note">Start with main.wapi</span>
                      </span>
                      <svg class="start-action-arrow ui-icon" viewBox="0 0 24 24" aria-hidden="true">
                        <path d="M5 12h14"></path>
                        <path d="m13 6 6 6-6 6"></path>
                      </svg>
                    </button>
                    <button id="startUploadProject" class="start-action" type="button">
                      <span class="start-action-icon" aria-hidden="true">
                        <svg class="ui-icon" viewBox="0 0 24 24">
                          <path d="M3 6.5h6l2 2h10v9.5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"></path>
                          <path d="M3 10h18"></path>
                        </svg>
                      </span>
                      <span class="start-action-copy">
                        <span class="start-action-title">Upload project</span>
                        <span class="start-action-note">Open a Wapi folder</span>
                      </span>
                      <svg class="start-action-arrow ui-icon" viewBox="0 0 24 24" aria-hidden="true">
                        <path d="M5 12h14"></path>
                        <path d="m13 6 6 6-6 6"></path>
                      </svg>
                    </button>
                  </div>
                </div>
              </section>
              <div id="monacoEditor"></div>
            </div>
            <section id="terminalPanel" class="terminal-panel" aria-label="Terminals">
              <div class="terminal-header">
                <span>TERMINALS</span>
                <div class="terminal-actions">
                  <button id="terminalCollapse" class="terminal-action" type="button" aria-label="Collapse terminals" title="Collapse terminals" aria-pressed="false">
                    <svg class="ui-icon" viewBox="0 0 24 24" aria-hidden="true">
                      <path d="m18 15-6-6-6 6"></path>
                    </svg>
                  </button>
                  <button id="terminalClose" class="terminal-action" type="button" aria-label="Close terminals" title="Close terminals">
                    <svg class="ui-icon" viewBox="0 0 24 24" aria-hidden="true">
                      <path d="M18 6 6 18"></path>
                      <path d="m6 6 12 12"></path>
                    </svg>
                  </button>
                </div>
              </div>
              <div class="terminal-empty">
                <div>
                  <strong>No terminals available</strong>
                  <span>Terminals are created automatically when a Wapi session starts</span>
                </div>
              </div>
            </section>
          </div>
        </section>
      </section>
      <footer class="status-bar" aria-label="Status">
        <div class="status-left">
          <span id="statusFile" class="status-item">&lt;/&gt; Welcome</span>
        </div>
        <div class="status-right">
          <span id="statusCursor" class="status-item">Ln 1, Col 1</span>
          <span class="status-item">Spaces: 4</span>
          <span class="status-item">UTF-8</span>
          <span id="statusLanguage" class="status-item">Wapi</span>
          <button id="statusTerminal" class="status-button" type="button">Terminal</button>
          <span class="status-dot" aria-hidden="true"></span>
          <span id="statusInstance" class="status-item">No Instance</span>
        </div>
      </footer>
      <div id="newProjectDialog" class="project-dialog" aria-hidden="true">
        <form id="newProjectForm" class="project-dialog-card">
          <h2>Create a new project</h2>
          <label class="project-field">
            <span>Project name</span>
            <input id="newProjectName" type="text" autocomplete="off" spellcheck="false" value="WapiProject">
          </label>
          <div class="project-dialog-actions">
            <button id="newProjectCancel" class="dialog-button" type="button">Cancel</button>
            <button class="dialog-button dialog-button-primary" type="submit">Create</button>
          </div>
        </form>
      </div>
    </main>
  `;

  document.getElementById("windowMinimize")?.addEventListener("click", () => bridge.window.minimize());
  document.getElementById("windowMaximize")?.addEventListener("click", () => bridge.window.toggleMaximize());
  document.getElementById("windowClose")?.addEventListener("click", () => bridge.window.close());
  document.getElementById("explorerCreateProject")?.addEventListener("click", () => setProjectDialogOpen(true));
  document.getElementById("explorerUploadProject")?.addEventListener("click", uploadProjectIntoExplorer);
  document.getElementById("explorerRefresh")?.addEventListener("click", refreshExplorerPanel);
  document.getElementById("explorerMoreActions")?.addEventListener("click", toggleExplorerMenu);
  document.getElementById("startCreateProject")?.addEventListener("click", () => setProjectDialogOpen(true));
  document.getElementById("startUploadProject")?.addEventListener("click", uploadProjectIntoExplorer);
  document.getElementById("newProjectCancel")?.addEventListener("click", () => setProjectDialogOpen(false));
  document.getElementById("newProjectDialog")?.addEventListener("click", (event) => {
    if (event.target.id === "newProjectDialog") setProjectDialogOpen(false);
  });
  document.getElementById("newProjectForm")?.addEventListener("submit", async (event) => {
    event.preventDefault();
    await createProjectFromDialog();
  });
  document.querySelectorAll(".activity-button[data-panel]").forEach((button) => {
    button.addEventListener("click", () => setActivePanel(button.dataset.panel));
  });
  document.getElementById("explorerSearch")?.addEventListener("input", (event) => {
    explorerState.searchQuery = event.target.value;
    renderSolutionExplorer();
  });
  document.getElementById("explorerActionMenu")?.addEventListener("click", async (event) => {
    const item = event.target.closest("[data-menu-action]");
    if (!item) return;
    uiState.menuOpen = false;
    if (item.dataset.menuAction === "create-project") setProjectDialogOpen(true);
    if (item.dataset.menuAction === "upload-project") await uploadProjectIntoExplorer();
    if (item.dataset.menuAction === "add-files") await addFilesToExplorer();
    if (item.dataset.menuAction === "clear-search") {
      explorerState.searchQuery = "";
      renderSolutionExplorer();
      setStatusMessage("Search cleared");
    }
  });
  document.getElementById("explorerFileTree")?.addEventListener("click", (event) => {
    const action = event.target.closest("[data-action]");
    if (action) {
      handlePanelAction(action.dataset.action);
      return;
    }
    const item = event.target.closest(".explorer-file");
    if (!item) return;
    explorerState.activeFileId = item.dataset.fileId;
    renderSolutionExplorer();
    setEditorModel(activeExplorerFile());
  });
  document.getElementById("terminalCollapse")?.addEventListener("click", () => {
    setTerminalState({ terminalCollapsed: !uiState.terminalCollapsed, terminalHidden: false });
  });
  document.getElementById("terminalClose")?.addEventListener("click", () => {
    setTerminalState({ terminalHidden: true, terminalCollapsed: false });
    renderSolutionExplorer();
  });
  document.getElementById("statusTerminal")?.addEventListener("click", () => {
    setTerminalState({ terminalHidden: !uiState.terminalHidden, terminalCollapsed: false });
    renderSolutionExplorer();
  });
  renderSolutionExplorer();
  setTerminalState();
  createMonacoEditor();
}

installRendererIcon();
installStyles();
installMonacoLanguage();
renderWindowBar();
