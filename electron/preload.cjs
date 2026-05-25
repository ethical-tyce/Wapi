const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("wapi", {
  execute: (payload) => ipcRenderer.invoke("wapi:execute", payload),
  locate: () => ipcRenderer.invoke("wapi:locate"),
  openFile: () => ipcRenderer.invoke("wapi:openFile"),
  saveFile: (payload) => ipcRenderer.invoke("wapi:saveFile", payload),
  window: {
    minimize: () => ipcRenderer.invoke("window:minimize"),
    toggleMaximize: () => ipcRenderer.invoke("window:toggleMaximize"),
    close: () => ipcRenderer.invoke("window:close")
  }
});
