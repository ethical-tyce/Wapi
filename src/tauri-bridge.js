import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

const native = Boolean(window.__TAURI_INTERNALS__);

const browserProject = (payload = {}) => ({
  rootPath: null,
  config: payload.config ?? null,
  files: payload.files ?? []
});

async function call(command, args, fallback) {
  if (!native) return typeof fallback === "function" ? fallback() : fallback;
  try {
    return await invoke(command, args);
  } catch (error) {
    throw new Error(typeof error === "string" ? error : error?.message || String(error));
  }
}

const bridge = {
  native,
  execute: (payload = {}) => call("execute", { payload }, {
    ok: false,
    code: null,
    stdout: "",
    stderr: `Native Wapi ${payload.command ?? "run"} is available in the Tauri app, not the browser preview.`,
    exe: null,
    pid: null,
    durationMs: 0,
    timedOut: false,
    outputTruncated: false
  }),
  locate: () => call("locate", {}, null),
  addFiles: () => call("add_files", {}, []),
  createProject: (payload = {}) => call("create_project", { payload }, () => browserProject(payload)),
  loadProject: (rootPath) => call("load_project", { rootPath: rootPath ?? null }, null),
  openFile: () => call("open_file", {}, null),
  saveFile: (payload = {}) => call(
    "save_file",
    { payload },
    { filePath: payload.filePath ?? "browser-preview.wapi" }
  ),
  saveFiles: (files = []) => call(
    "save_files",
    { files },
    files.map((file) => ({ ok: true, filePath: file.filePath, error: null }))
  ),
  readProjectConfig: (rootPath) => call("read_project_config", { rootPath }, null),
  writeProjectConfig: (rootPath, config) => call(
    "write_project_config",
    { rootPath, config },
    config
  ),
  listRecentProjects: () => call("list_recent_projects", {}, []),
  addRecentProject: (rootPath) => call("add_recent_project", { rootPath }, []),
  shell: (payload = {}) => call("shell", { payload }, {
    ok: true,
    code: 0,
    stdout: "",
    stderr: "",
    cwd: payload.cwd ?? "",
    shell: payload.shell ?? "powershell"
  }),
  terminal: {
    start: (payload = {}) => call("terminal_start", { payload }, {
      ok: true,
      sessionId: payload.sessionId,
      shell: payload.shell ?? "powershell",
      cwd: payload.cwd ?? "",
      pid: null,
      stderr: null
    }),
    send: (payload = {}) => call("terminal_send", { payload }, { ok: true }),
    resize: (payload = {}) => call("terminal_resize", { payload }, { ok: true }),
    stop: (payload = {}) => call("terminal_stop", { payload }, { ok: true }),
    onData: (callback) => {
      if (!native) return () => undefined;
      let disposed = false;
      let unlisten = null;
      listen("wapi-terminal-data", (event) => callback(event.payload)).then((cleanup) => {
        if (disposed) cleanup();
        else unlisten = cleanup;
      });
      return () => {
        disposed = true;
        unlisten?.();
      };
    }
  },
  window: {
    minimize: () => call("window_minimize", {}, null),
    toggleMaximize: () => call("window_toggle_maximize", {}, null),
    close: () => call("window_close", {}, null),
    setDirtyState: (isDirty) => call("window_set_dirty_state", { isDirty }, true)
  }
};

export default bridge;
