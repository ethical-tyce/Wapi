const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("wapi", {
  execute: (payload) => ipcRenderer.invoke("wapi:execute", payload),
  locate: () => ipcRenderer.invoke("wapi:locate"),
  addFiles: () => ipcRenderer.invoke("wapi:addFiles"),
  loadProject: () => ipcRenderer.invoke("wapi:loadProject"),
  openFile: () => ipcRenderer.invoke("wapi:openFile"),
  saveFile: (payload) => ipcRenderer.invoke("wapi:saveFile", payload),
  shell: (payload) => ipcRenderer.invoke("wapi:shell", payload),
  terminal: {
    start: (payload) => ipcRenderer.invoke("wapi:terminal:start", payload),
    send: (payload) => ipcRenderer.invoke("wapi:terminal:send", payload),
    stop: () => ipcRenderer.invoke("wapi:terminal:stop"),
    onData: (callback) => {
      const listener = (_event, payload) => callback(payload);
      ipcRenderer.on("wapi:terminal:data", listener);
      return () => ipcRenderer.removeListener("wapi:terminal:data", listener);
    }
  },
  window: {
    minimize: () => ipcRenderer.invoke("window:minimize"),
    toggleMaximize: () => ipcRenderer.invoke("window:toggleMaximize"),
    close: () => ipcRenderer.invoke("window:close")
  }
});
