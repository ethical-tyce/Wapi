import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import {
  Activity,
  BookOpen,
  Boxes,
  CheckCircle2,
  ChevronDown,
  Code2,
  FileCode2,
  FilePlus2,
  Folder,
  FolderOpen,
  FolderTree,
  ListChecks,
  Maximize2,
  Minus,
  Play,
  Save,
  Search,
  Shield,
  Terminal,
  X,
  XCircle
} from "lucide-react";
import * as monaco from "monaco-editor/esm/vs/editor/editor.api";
import "monaco-editor/min/vs/editor/editor.main.css";
import editorWorker from "monaco-editor/esm/vs/editor/editor.worker?worker";
import wapiIconUrl from "../wapi.png";
import "./styles.css";

self.MonacoEnvironment = {
  getWorker() {
    return new editorWorker();
  }
};

const defaultSource = `int pid = findProcessPID("notepad")
int handle = openProcess(pid)
suspendProcess(handle)
resumeProcess(handle)`;

const dllMainSource = `#include <windows.h>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }

    return TRUE;
}`;

const payloadSource = `#include "payload.h"
#include <windows.h>

void runPayload() {
    MessageBoxW(nullptr, L"Wapi DLL loaded", L"Wapi", MB_OK);
}`;

const payloadHeaderSource = `#pragma once

void runPayload();`;

const functionSignatures = {
  listProcesses: "listProcesses()",
  findProcessPID: "findProcessPID(name)",
  openProcess: "openProcess(pid)",
  terminateProcess: "terminateProcess(handle)",
  suspendProcess: "suspendProcess(handle)",
  resumeProcess: "resumeProcess(handle)",
  readMemory: "readMemory(handle, address)",
  writeMemory: "writeMemory(handle, address, value)",
  allocMemory: "allocMemory(handle, size)",
  freeMemory: "freeMemory(handle, address)",
  findWindow: "findWindow(windowTitle)",
  injectDLL: "injectDLL(pid, dllPath)",
  testInjectDLL: "testInjectDLL(pid)"
};

const knowledgeGroups = [
  {
    title: "Process",
    items: [
      { name: "listProcesses", requirement: "proc.list", note: "Enumerates visible Windows processes." },
      { name: "findProcessPID", requirement: "proc.list", note: "Finds a PID from a process image name." },
      { name: "openProcess", requirement: "proc.open.all_access", note: "Returns a process handle from a PID." },
      { name: "terminateProcess", requirement: "proc.terminate", note: "Terminates the target process handle." },
      { name: "suspendProcess", requirement: "proc.suspend", note: "Suspends the target process handle." },
      { name: "resumeProcess", requirement: "proc.resume", note: "Resumes the target process handle." }
    ]
  },
  {
    title: "Memory",
    items: [
      { name: "readMemory", requirement: "mem.read", note: "Reads an integer value from a target address." },
      { name: "writeMemory", requirement: "mem.write", note: "Writes an integer value to a target address." },
      { name: "allocMemory", requirement: "mem.alloc", note: "Allocates memory inside the target process." },
      { name: "freeMemory", requirement: "mem.free", note: "Frees memory allocated in the target process." }
    ]
  },
  {
    title: "Window",
    items: [
      { name: "findWindow", requirement: "window.find", note: "Finds a top-level window by title." }
    ]
  },
  {
    title: "Injection",
    items: [
      { name: "injectDLL", requirement: "inject.dll + --allow-injection", note: "Injects a DLL path into a process." },
      { name: "testInjectDLL", requirement: "inject.dll + --allow-injection", note: "Loads TestDLL.dll next to Wapi.exe." }
    ]
  }
];

const runtimeRequirements = [
  "Build Wapi.exe before running scripts from the IDE.",
  "Use check for preflight; run can perform real process and memory actions.",
  "Strict permissions require matching --cap values for every called API.",
  "DLL injection requires --allow-injection unless mode is unsafe.",
  "testInjectDLL(pid) expects TestDLL.dll beside the built Wapi.exe."
];

const capabilities = [
  "proc.list",
  "proc.open.all_access",
  "proc.terminate",
  "proc.suspend",
  "proc.resume",
  "mem.read",
  "mem.write",
  "mem.alloc",
  "mem.free",
  "window.find",
  "inject.dll"
];

const allFunctions = Object.keys(functionSignatures);
const pathDivider = /[\\/]/;
const bridge = window.wapi ?? {
  execute: async () => ({
    ok: false,
    code: null,
    stdout: "",
    stderr: "Open this screen in Electron to execute Wapi scripts.",
    exe: null
  }),
  locate: async () => null,
  addFiles: async () => [],
  loadProject: async () => ({ rootPath: null, files: [] }),
  openFile: async () => null,
  saveFile: async ({ filePath }) => ({ filePath: filePath || null }),
  window: {
    minimize: async () => null,
    toggleMaximize: async () => null,
    close: async () => null
  }
};

function installRendererIcon() {
  const existing = document.querySelector("link[rel='icon']");
  const link = existing || document.createElement("link");
  link.rel = "icon";
  link.type = "image/png";
  link.href = wapiIconUrl;
  if (!existing) document.head.appendChild(link);
}

function registerWapiLanguage() {
  if (!monaco.languages.getLanguages().some((language) => language.id === "wapi-cpp")) {
    monaco.languages.register({ id: "wapi-cpp", extensions: [".cpp", ".h"], aliases: ["Wapi DLL C++"] });
    monaco.languages.setMonarchTokensProvider("wapi-cpp", {
      keywords: ["BOOL", "DWORD", "HMODULE", "LPVOID", "nullptr", "switch", "case", "break", "return", "void", "include"],
      tokenizer: {
        root: [
          [/\/\/.*$/, "comment"],
          [/#\s*include\b/, "cpp-keyword"],
          [/".*?"/, "string"],
          [/L".*?"/, "string"],
          [/\b[A-Z_][A-Z0-9_]*\b/, "cpp-type"],
          [/[a-zA-Z_]\w*(?=\s*\()/, "function"],
          [/[a-zA-Z_]\w*/, {
            cases: {
              "@keywords": "cpp-keyword",
              "@default": "identifier"
            }
          }],
          [/[{}()[\];,.]/, "delimiter"]
        ]
      }
    });
  }

  if (monaco.languages.getLanguages().some((language) => language.id === "wapi")) return;

  monaco.languages.register({ id: "wapi", extensions: [".wapi"], aliases: ["Wapi"] });

  monaco.languages.setLanguageConfiguration("wapi", {
    brackets: [["(", ")"], ["{", "}"]],
    autoClosingPairs: [
      { open: "(", close: ")" },
      { open: "{", close: "}" },
      { open: "\"", close: "\"" }
    ],
    surroundingPairs: [
      { open: "(", close: ")" },
      { open: "{", close: "}" },
      { open: "\"", close: "\"" }
    ]
  });

  monaco.languages.setMonarchTokensProvider("wapi", {
    keywords: ["int", "long", "string", "bool", "if", "else", "while"],
    tokenizer: {
      root: [
        [/".*?"/, "string"],
        [/0x[0-9a-fA-F]+/, "number.hex"],
        [/\b\d+\b/, "number"],
        [/[a-zA-Z_]\w*(?=\s*\()/, {
          cases: {
            "@keywords": "keyword",
            "@default": "function"
          }
        }],
        [/[a-zA-Z_]\w*/, {
          cases: {
            "@keywords": "keyword",
            "@default": "identifier"
          }
        }],
        [/[(){}.,=]/, "delimiter"]
      ]
    }
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

      const functionItems = allFunctions.map((name) => ({
        label: name,
        kind: monaco.languages.CompletionItemKind.Function,
        detail: functionSignatures[name],
        insertText: `${name}($0)`,
        insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
        range
      }));

      const keywordItems = ["int", "long", "string", "bool", "if", "else", "while"].map((keyword) => ({
        label: keyword,
        kind: monaco.languages.CompletionItemKind.Keyword,
        insertText: keyword,
        range
      }));

      return { suggestions: [...functionItems, ...keywordItems] };
    }
  });

  monaco.editor.defineTheme("wapi-black", {
    base: "vs-dark",
    inherit: true,
    rules: [
      { token: "keyword", foreground: "6dff9b", fontStyle: "bold" },
      { token: "function", foreground: "b7ffc9" },
      { token: "number", foreground: "83d6ff" },
      { token: "number.hex", foreground: "83d6ff" },
      { token: "string", foreground: "f0d58a" },
      { token: "identifier", foreground: "dfeee4" },
      { token: "comment", foreground: "526259" },
      { token: "cpp-keyword", foreground: "6dff9b", fontStyle: "bold" },
      { token: "cpp-type", foreground: "83d6ff" },
      { token: "delimiter", foreground: "627166" }
    ],
    colors: {
      "editor.background": "#020403",
      "editor.foreground": "#dfeee4",
      "editorLineNumber.foreground": "#2a362f",
      "editorLineNumber.activeForeground": "#8cffad",
      "editorCursor.foreground": "#6dff9b",
      "editor.selectionBackground": "#173e28",
      "editor.inactiveSelectionBackground": "#0f2519",
      "editor.lineHighlightBackground": "#07110c",
      "editorIndentGuide.background1": "#102019",
      "editorIndentGuide.activeBackground1": "#31513d",
      "editorSuggestWidget.background": "#050806",
      "editorSuggestWidget.border": "#193823",
      "editorSuggestWidget.selectedBackground": "#0d2d19",
      "editorWidget.background": "#050806",
      "editorWidget.border": "#193823",
      "input.background": "#050806",
      "focusBorder": "#6dff9b"
    }
  });
}

function getLineColumnFromOffset(source, offset) {
  const lines = source.slice(0, offset).split("\n");
  return {
    lineNumber: lines.length,
    column: lines[lines.length - 1].length + 1
  };
}

function languageFromFileName(fileName = "") {
  return /\.(cpp|c|h|hpp)$/i.test(fileName) ? "wapi-cpp" : "wapi";
}

function normalizeProjectFile(file, fallback = {}) {
  const name = file.name || file.filePath?.split(pathDivider).pop() || "untitled.wapi";
  const id = file.id || file.filePath || `${fallback.prefix || "virtual"}:${name}:${Date.now()}`;
  const relativePath = file.relativePath || name;
  const parts = relativePath.split(pathDivider);
  return {
    id,
    name,
    relativePath,
    section: parts.length > 1 ? parts.slice(0, -1).join("/") : fallback.section || "Loose Files",
    filePath: file.filePath || null,
    source: file.source || "",
    language: file.language || languageFromFileName(name),
    readonly: Boolean(file.readonly)
  };
}

function groupProjectFiles(files) {
  const groups = new Map();
  files.forEach((file) => {
    const section = file.section || "Loose Files";
    if (!groups.has(section)) groups.set(section, []);
    groups.get(section).push(file);
  });

  return [...groups.entries()].map(([name, groupedFiles]) => ({
    name,
    files: groupedFiles.sort((a, b) => a.name.localeCompare(b.name))
  }));
}

function analyzeSource(source) {
  const markers = [];
  const knownWords = new Set([
    "int",
    "long",
    "string",
    "bool",
    "if",
    "else",
    "while",
    ...allFunctions
  ]);

  source.split("\n").forEach((line, index) => {
    const quotes = [...line.matchAll(/"/g)].length;
    if (quotes % 2 === 1) {
      markers.push({
        severity: monaco.MarkerSeverity.Error,
        message: "Unclosed string literal.",
        startLineNumber: index + 1,
        startColumn: Math.max(1, line.lastIndexOf("\"") + 1),
        endLineNumber: index + 1,
        endColumn: line.length + 1
      });
    }
  });

  for (const match of source.matchAll(/\b([a-zA-Z_]\w*)\s*\(/g)) {
    const name = match[1];
    if (!knownWords.has(name)) {
      const start = getLineColumnFromOffset(source, match.index);
      markers.push({
        severity: monaco.MarkerSeverity.Warning,
        message: `Unknown Wapi function '${name}'.`,
        startLineNumber: start.lineNumber,
        startColumn: start.column,
        endLineNumber: start.lineNumber,
        endColumn: start.column + name.length
      });
    }
  }

  return markers;
}

function IconButton({ title, children, onClick, active = false, disabled = false }) {
  return (
    <button
      className={`icon-button${active ? " active" : ""}`}
      onClick={onClick}
      disabled={disabled}
      aria-label={title}
      title={title}
    >
      {children}
    </button>
  );
}

function Toggle({ checked, onChange, label }) {
  return (
    <button className={`toggle${checked ? " checked" : ""}`} onClick={() => onChange(!checked)} type="button">
      <span className="toggle-track">
        <span className="toggle-knob" />
      </span>
      <span>{label}</span>
    </button>
  );
}

function SelectControl({ value, onChange, options }) {
  return (
    <label className="select-wrap">
      <Shield size={14} />
      <select value={value} onChange={(event) => onChange(event.target.value)}>
        {options.map((option) => (
          <option key={option} value={option}>
            {option}
          </option>
        ))}
      </select>
      <ChevronDown size={14} />
    </label>
  );
}

function StatusPill({ result, running, executable }) {
  if (running) {
    return (
      <span className="status-pill active">
        <Activity size={14} />
        Running
      </span>
    );
  }

  if (!result) {
    return (
      <span className="status-pill">
        <Terminal size={14} />
        {executable ? "Ready" : "No Wapi.exe"}
      </span>
    );
  }

  return (
    <span className={`status-pill ${result.ok ? "success" : "error"}`}>
      {result.ok ? <CheckCircle2 size={14} /> : <XCircle size={14} />}
      {result.ok ? "Clean" : `Exit ${result.code ?? "error"}`}
    </span>
  );
}

function WindowControls() {
  return (
    <div className="window-controls">
      <button type="button" aria-label="Minimize" title="Minimize" onClick={() => bridge.window.minimize()}>
        <Minus size={15} />
      </button>
      <button type="button" aria-label="Maximize" title="Maximize" onClick={() => bridge.window.toggleMaximize()}>
        <Maximize2 size={14} />
      </button>
      <button className="close" type="button" aria-label="Close" title="Close" onClick={() => bridge.window.close()}>
        <X size={15} />
      </button>
    </div>
  );
}

function SolutionExplorer({ activeFileId, projectSections, onOpenVirtualFile, onAddFiles, onLoadProject, onNewScript, onNewDll }) {
  return (
    <aside className="sidebar">
      <div className="brand-block">
        <div className="brand-mark">
          <img src={wapiIconUrl} alt="Wapi" />
        </div>
        <div>
          <h1>Wapi</h1>
          <p>Solution Explorer</p>
        </div>
      </div>

      <section>
        <div className="section-title-row">
          <h2>Solution</h2>
          <FolderTree size={14} />
        </div>
        <div className="explorer-actions">
          <button onClick={onAddFiles} type="button" title="Add existing files">
            <FilePlus2 size={14} />
            Add
          </button>
          <button onClick={onLoadProject} type="button" title="Load a project folder">
            <FolderOpen size={14} />
            Load
          </button>
          <button onClick={onNewScript} type="button" title="New Wapi script">
            <Code2 size={14} />
            Wapi
          </button>
          <button onClick={onNewDll} type="button" title="New custom DLL file set">
            <Boxes size={14} />
            DLL
          </button>
        </div>
      </section>

      <section className="explorer-tree">
        {projectSections.length === 0 ? (
          <div className="empty-explorer">No files loaded.</div>
        ) : (
          projectSections.map((section) => (
            <div className="explorer-folder" key={section.name}>
              <div className="folder-row">
                <Folder size={14} />
                <span>{section.name}</span>
              </div>
              <div className="file-stack">
                {section.files.map((file) => (
                  <button
                    key={file.id}
                    className={activeFileId === file.id ? "selected" : ""}
                    onClick={() => onOpenVirtualFile(file)}
                    type="button"
                  >
                    <FileCode2 size={13} />
                    <span>{file.name}</span>
                  </button>
                ))}
              </div>
            </div>
          ))
        )}
      </section>
    </aside>
  );
}

function KnowledgeBase({ selectedCapabilities, onCapabilitiesChange, onInsertFunction }) {
  const [query, setQuery] = useState("");
  const normalizedQuery = query.trim().toLowerCase();
  const filteredGroups = useMemo(() => {
    if (!normalizedQuery) return knowledgeGroups;
    return knowledgeGroups
      .map((group) => ({
        ...group,
        items: group.items.filter((item) => {
          const signature = functionSignatures[item.name] || item.name;
          return [group.title, item.name, signature, item.requirement, item.note]
            .join(" ")
            .toLowerCase()
            .includes(normalizedQuery);
        })
      }))
      .filter((group) => group.items.length > 0);
  }, [normalizedQuery]);

  return (
    <section className="knowledge-panel">
      <div className="panel-head">
        <div>
          <BookOpen size={15} />
          Knowledge Base
        </div>
        <span>Wapi API</span>
      </div>
      <label className="knowledge-search">
        <Search size={14} />
        <input
          value={query}
          onChange={(event) => setQuery(event.target.value)}
          placeholder="Search functions..."
          type="search"
        />
      </label>
      <div className="knowledge-content">
        <div className="kb-block">
          <h2>
            <ListChecks size={14} />
            Requirements
          </h2>
          <ul className="runtime-list">
            {runtimeRequirements.map((requirement) => (
              <li key={requirement}>{requirement}</li>
            ))}
          </ul>
        </div>

        <div className="kb-block">
          <h2>
            <Shield size={14} />
            Capability Toggles
          </h2>
          <CapabilityPicker selected={selectedCapabilities} onChange={onCapabilitiesChange} />
        </div>

        {filteredGroups.length === 0 ? <div className="empty-explorer">No matching functions.</div> : null}

        {filteredGroups.map((group) => (
          <div className="kb-group" key={group.title}>
            <h2>
              <Code2 size={14} />
              {group.title}
            </h2>
            {group.items.map((item) => (
              <button className="kb-function" key={item.name} onClick={() => onInsertFunction(item.name)} type="button">
                <span className="kb-signature">{functionSignatures[item.name]}</span>
                <span className="requirement-badge">{item.requirement}</span>
                <span className="kb-note">{item.note}</span>
              </button>
            ))}
          </div>
        ))}
      </div>
    </section>
  );
}

function OutputPanel({ result, running, executable }) {
  const output = useMemo(() => {
    if (running) return "Executing Wapi...";
    if (!result) {
      return executable
        ? `Executable: ${executable}`
        : "Build Wapi.exe or set WAPI_EXE to enable run/check.";
    }

    const parts = [];
    if (result.exe) parts.push(`Executable: ${result.exe}`);
    if (result.stdout) parts.push(result.stdout.trimEnd());
    if (result.stderr) parts.push(result.stderr.trimEnd());
    if (!result.stdout && !result.stderr) parts.push(`Process exited with code ${result.code}.`);
    return parts.join("\n\n");
  }, [executable, result, running]);

  return (
    <section className="output-panel">
      <div className="panel-head">
        <div>
          <Terminal size={15} />
          Output
        </div>
        {result && !running ? <span>{result.ok ? "ok" : "failed"}</span> : null}
      </div>
      <pre>{output}</pre>
    </section>
  );
}

function CapabilityPicker({ selected, onChange }) {
  const toggleCapability = (capability) => {
    if (selected.includes(capability)) {
      onChange(selected.filter((item) => item !== capability));
      return;
    }
    onChange([...selected, capability]);
  };

  return (
    <div className="capability-grid">
      {capabilities.map((capability) => (
        <button
          key={capability}
          className={selected.includes(capability) ? "selected" : ""}
          onClick={() => toggleCapability(capability)}
          type="button"
        >
          {capability}
        </button>
      ))}
    </div>
  );
}

function App() {
  const editorNode = useRef(null);
  const editorRef = useRef(null);
  const modelRef = useRef(null);
  const activeFileIdRef = useRef(null);
  const [source, setSource] = useState("");
  const [filePath, setFilePath] = useState(null);
  const [activeFile, setActiveFile] = useState("untitled.wapi");
  const [activeFileId, setActiveFileId] = useState(null);
  const [projectFiles, setProjectFiles] = useState([]);
  const [currentLanguage, setCurrentLanguage] = useState("wapi");
  const [readOnly, setReadOnly] = useState(false);
  const [mode, setMode] = useState("safe");
  const [strictPermissions, setStrictPermissions] = useState(false);
  const [allowInjection, setAllowInjection] = useState(false);
  const [selectedCapabilities, setSelectedCapabilities] = useState(["proc.list"]);
  const [command, setCommand] = useState("check");
  const [running, setRunning] = useState(false);
  const [result, setResult] = useState(null);
  const [executable, setExecutable] = useState(null);
  const [dirty, setDirty] = useState(false);

  useEffect(() => {
    activeFileIdRef.current = activeFileId;
  }, [activeFileId]);

  useEffect(() => {
    installRendererIcon();
    registerWapiLanguage();

    const model = monaco.editor.createModel(source, currentLanguage);
    modelRef.current = model;

    const editor = monaco.editor.create(editorNode.current, {
      model,
      theme: "wapi-black",
      automaticLayout: true,
      fontFamily: "\"SF Mono\", \"Cascadia Code\", Consolas, monospace",
      fontSize: 14,
      lineHeight: 23,
      minimap: { enabled: false },
      scrollBeyondLastLine: false,
      renderLineHighlight: "all",
      overviewRulerBorder: false,
      padding: { top: 18, bottom: 18 },
      roundedSelection: false,
      cursorBlinking: "smooth",
      smoothScrolling: true,
      wordWrap: "on",
      readOnly,
      tabSize: 4,
      glyphMargin: false,
      folding: false,
      lineDecorationsWidth: 10,
      lineNumbersMinChars: 3
    });

    editorRef.current = editor;
    monaco.editor.setModelMarkers(model, "wapi", analyzeSource(source));

    const subscription = model.onDidChangeContent(() => {
      const value = model.getValue();
      setSource(value);
      setDirty(true);
      const currentFileId = activeFileIdRef.current;
      setProjectFiles((files) =>
        currentFileId ? files.map((file) => (file.id === currentFileId ? { ...file, source: value } : file)) : files
      );
      monaco.editor.setModelMarkers(model, "wapi", analyzeSource(value));
    });

    bridge.locate().then(setExecutable);

    return () => {
      subscription.dispose();
      editor.dispose();
      model.dispose();
    };
  }, []);

  const projectSections = useMemo(() => groupProjectFiles(projectFiles), [projectFiles]);

  const mergeProjectFiles = useCallback((incomingFiles, options = {}) => {
    const normalized = incomingFiles.map((file) => normalizeProjectFile(file, options));
    setProjectFiles((files) => {
      const byId = new Map(files.map((file) => [file.id, file]));
      normalized.forEach((file) => byId.set(file.id, file));
      return [...byId.values()];
    });
    return normalized;
  }, []);

  const replaceProjectFiles = useCallback((incomingFiles, options = {}) => {
    const normalized = incomingFiles.map((file) => normalizeProjectFile(file, options));
    setProjectFiles(normalized);
    return normalized;
  }, []);

  const replaceSource = useCallback((nextSource, options = {}) => {
    const model = modelRef.current;
    if (!model) return;
    const nextLanguage = options.language || "wapi";
    const nextReadOnly = Boolean(options.readOnly);
    activeFileIdRef.current = options.fileId || null;
    model.pushEditOperations([], [{ range: model.getFullModelRange(), text: nextSource }], () => null);
    monaco.editor.setModelLanguage(model, nextLanguage);
    editorRef.current?.updateOptions({ readOnly: nextReadOnly });
    setCurrentLanguage(nextLanguage);
    setReadOnly(nextReadOnly);
    if (options.fileName) setActiveFile(options.fileName);
    setActiveFileId(options.fileId || null);
    if (options.clearDiskFile) setFilePath(null);
    if (options.markDirty === false) setDirty(false);
    if (options.markDirty === true) setDirty(true);
    editorRef.current?.focus();
  }, []);

  const insertFunction = useCallback((name) => {
    const editor = editorRef.current;
    if (!editor) return;
    editor.trigger("keyboard", "type", { text: functionSignatures[name] });
    editor.focus();
  }, []);

  const runCurrent = useCallback(async (nextCommand = command) => {
    setRunning(true);
    setCommand(nextCommand);
    setResult(null);

    const response = await bridge.execute({
      command: nextCommand,
      source,
      options: {
        mode,
        strictPermissions,
        allowInjection,
        capabilities: selectedCapabilities
      }
    });

    setResult(response);
    if (response.exe) setExecutable(response.exe);
    setRunning(false);
  }, [allowInjection, command, mode, selectedCapabilities, source, strictPermissions]);

  const openFile = useCallback(async () => {
    const opened = await bridge.openFile();
    if (!opened) return;
    const [file] = mergeProjectFiles([opened]);
    setFilePath(opened.filePath);
    replaceSource(opened.source, {
      fileId: file.id,
      fileName: file.name,
      language: file.language,
      markDirty: false
    });
    setDirty(false);
  }, [mergeProjectFiles, replaceSource]);

  const addFiles = useCallback(async () => {
    const files = await bridge.addFiles();
    if (!files?.length) return;
    const [firstFile] = mergeProjectFiles(files);
    if (firstFile && !activeFileId) {
      setFilePath(firstFile.filePath);
      replaceSource(firstFile.source, {
        fileId: firstFile.id,
        fileName: firstFile.name,
        language: firstFile.language,
        markDirty: false
      });
    }
  }, [activeFileId, mergeProjectFiles, replaceSource]);

  const loadProject = useCallback(async () => {
    const project = await bridge.loadProject();
    if (!project) return;
    const files = replaceProjectFiles(project.files || [], { prefix: project.rootPath || "project" });
    const firstFile = files[0];
    if (firstFile) {
      setFilePath(firstFile.filePath);
      replaceSource(firstFile.source, {
        fileId: firstFile.id,
        fileName: firstFile.name,
        language: firstFile.language,
        markDirty: false
      });
    } else {
      setFilePath(null);
      replaceSource("", {
        fileName: "untitled.wapi",
        language: "wapi",
        markDirty: false
      });
    }
  }, [replaceProjectFiles, replaceSource]);

  const saveFile = useCallback(async () => {
    const saved = await bridge.saveFile({ filePath, source });
    if (!saved) return;
    const name = saved.filePath.split(pathDivider).pop();
    const savedFile = normalizeProjectFile({ filePath: saved.filePath, source, name });
    setFilePath(saved.filePath);
    setActiveFile(name);
    setProjectFiles((files) =>
      activeFileId
        ? files.map((file) =>
            file.id === activeFileId ? { ...file, ...savedFile } : file
          )
        : [...files, savedFile]
    );
    activeFileIdRef.current = saved.filePath;
    setActiveFileId(saved.filePath);
    setDirty(false);
  }, [activeFileId, filePath, source]);

  const openProjectFile = useCallback((file) => {
    setFilePath(file.filePath);
    replaceSource(file.source, {
      fileId: file.id,
      fileName: file.name,
      language: file.language,
      readOnly: file.readonly,
      clearDiskFile: !file.filePath,
      markDirty: false
    });
  }, [replaceSource]);

  const newScript = useCallback(() => {
    const [file] = mergeProjectFiles([{ name: "new-script.wapi", source: defaultSource, language: "wapi" }], {
      prefix: "new-script",
      section: "Unsaved"
    });
    replaceSource(file.source, {
      fileId: file.id,
      fileName: file.name,
      language: "wapi",
      clearDiskFile: true,
      markDirty: true
    });
  }, [mergeProjectFiles, replaceSource]);

  const newDll = useCallback(() => {
    const [file] = mergeProjectFiles([{ name: "dllmain.cpp", relativePath: "custom-dll/dllmain.cpp", source: dllMainSource, language: "wapi-cpp" }], {
      prefix: "new-dll",
      section: "Unsaved"
    });
    replaceSource(file.source, {
      fileId: file.id,
      fileName: file.name,
      language: "wapi-cpp",
      clearDiskFile: true,
      markDirty: true
    });
  }, [mergeProjectFiles, replaceSource]);

  const fileLabel = filePath ? filePath.split(/[\\/]/).pop() : activeFile;

  return (
    <main className="app-shell">
      <SolutionExplorer
        activeFileId={activeFileId}
        projectSections={projectSections}
        onOpenVirtualFile={openProjectFile}
        onAddFiles={addFiles}
        onLoadProject={loadProject}
        onNewScript={newScript}
        onNewDll={newDll}
      />

      <section className="workspace">
        <header className="topbar">
          <div className="window-drag">
            <span className="file-state">{dirty ? "modified" : "saved"}</span>
            <strong>{fileLabel}</strong>
          </div>

          <div className="toolbar">
            <IconButton title="Open script" onClick={openFile}>
              <FolderOpen size={16} />
            </IconButton>
            <IconButton title="Save script" onClick={saveFile}>
              <Save size={16} />
            </IconButton>
            <SelectControl value={mode} onChange={setMode} options={["safe", "dev", "unsafe"]} />
            <Toggle checked={strictPermissions} onChange={setStrictPermissions} label="strict" />
            <Toggle checked={allowInjection} onChange={setAllowInjection} label="inject" />
            <StatusPill result={result} running={running} executable={executable} />
            <button className="run-button ghost" onClick={() => runCurrent("check")} disabled={running} type="button">
              <CheckCircle2 size={16} />
              Check
            </button>
            <button className="run-button" onClick={() => runCurrent("run")} disabled={running} type="button">
              <Play size={16} />
              Run
            </button>
          </div>
          <WindowControls />
        </header>

        <section className="editor-grid">
          <div className="editor-card">
            <div className="editor-meta">
              <span>{currentLanguage === "wapi-cpp" ? "Wapi DLL Source" : "Wapi Script"}</span>
              <span>v0.01</span>
            </div>
            <div ref={editorNode} className="editor-host" />
          </div>

          <aside className="right-rail">
            <KnowledgeBase
              selectedCapabilities={selectedCapabilities}
              onCapabilitiesChange={setSelectedCapabilities}
              onInsertFunction={insertFunction}
            />
            <OutputPanel result={result} running={running} executable={executable} />
          </aside>
        </section>
      </section>
    </main>
  );
}

const rootElement = document.getElementById("root");
const root = window.__wapiRoot ?? createRoot(rootElement);
window.__wapiRoot = root;
root.render(<App />);
