# Wapi

Wapi is a C++ command-line language/runtime for controlled Windows API scripting.

It gives you a small custom language with variables, expressions, blocks, loops, and Windows API runtime calls that can:
- inspect processes,
- open process handles,
- read/write/allocate/free memory,
- find windows,
- and perform DLL injection (guarded by runtime policy flags).

## Current Status

This repository is an early version (`v0.01`) with:
- working lexer/parser/evaluator pipeline,
- a safety-oriented runtime policy model,
- implemented Windows API bindings for core process/memory/window/injection tasks,
- a built-in standardized test command (`wapi test`) with implemented + roadmap checks.

## Project Structure

- `native/src/` - C++ CLI, lexer, parser, evaluator, and Windows API runtime.
- `native/TestDLL/` - sample DLL project used by `testInjectDLL(pid)`.
- `native/Wapi.vcxproj` - main Visual Studio project.
- `src/` - Vite renderer for the desktop IDE.
- `src-tauri/` - Tauri host, guarded runtime execution, projects, and terminal integration.
- `docs/assets/` - application branding shared by the renderer and desktop bundle.
- `docs/design/` - design reference and visual QA notes.
- `Wapi.slnx` - root Visual Studio solution for the native projects.

## Language Snapshot

The language currently supports:
- variable declarations (for example `int pid = findProcessPID("notepad")`),
- assignment (for example `pid = pid + 1`),
- string/int/hex/bool literals,
- arithmetic and comparison expressions,
- `if`/`else` blocks,
- `while` loops,
- function calls as statements and expressions,
- namespaced aliases such as `proc.find(...)`, `proc.open(...)`, and `mem.read(...)`.

Example script:

```txt
int pid = findProcessPID("notepad")
if pid != 0 {
    long handle = proc.open(pid)
    proc.suspend(handle)
    proc.resume(handle)
    proc.close(handle)
}
```

Semicolons are optional, so this also works:

```txt
int i = 0;
while i < 3 {
    print("loop=" + i);
    i = i + 1;
}
```

## CLI Usage

```txt
wapi run "<script>" [--mode safe|dev|unsafe] [--allow-injection] [--strict-permissions] [--cap <name>]...
wapi check "<script>" [--mode safe|dev|unsafe] [--allow-injection] [--strict-permissions] [--cap <name>]...
wapi test
```

Examples:

```powershell
wapi run "int pid = findProcessPID(\"notepad\")" --mode safe
wapi check "int pid = findProcessPID(\"notepad\") testInjectDLL(pid)" --allow-injection
wapi test
```

## Safety Model

Wapi enforces policy at runtime with these controls:

- `--mode safe|dev|unsafe`
- `--allow-injection` (required for injection outside `unsafe` mode)
- `--strict-permissions`
- `--cap <capability>` (can be repeated)

In compatibility mode (no `--strict-permissions`), missing capabilities emit warnings and audit logs.
In strict mode, missing capabilities become hard errors.

Audit lines are emitted as `[WAPI_AUDIT] ...`.

## Implemented Functions

### Process
- `listProcesses()`
- `proc.list()`
- `findProcessPID(name)`
- `proc.find(name)`
- `openProcess(pid)`
- `proc.open(pid)`
- `terminateProcess(handle)`
- `proc.terminate(handle)`
- `suspendProcess(handle)`
- `proc.suspend(handle)`
- `resumeProcess(handle)`
- `proc.resume(handle)`
- `closeProcess(handle)`
- `proc.close(handle)`

### Memory
- `readMemory(handle, address)`
- `mem.read(handle, address)`
- `writeMemory(handle, address, value)`
- `mem.write(handle, address, value)`
- `allocMemory(handle, size)`
- `mem.alloc(handle, size)`
- `freeMemory(handle, address)`
- `mem.free(handle, address)`
- `protectMemory(handle, address, size, protection)`
- `mem.protect(handle, address, size, protection)`
- `queryMemory(handle, address)`
- `mem.query(handle, address)`

### Modules
- `listModules(pid)`
- `proc.modules(pid)`
- `getModuleBase(pid, moduleName)`
- `getModuleBaseAddress(pid, moduleName)`
- `getModuleSize(pid, moduleName)`
- `proc.module.base(pid, moduleName)`
- `proc.module.size(pid, moduleName)`

### Runtime
- `print(value)`

### Threads
- `listThreads(pid)`
- `openThread(tid)`
- `suspendThread(threadHandle)`
- `resumeThread(threadHandle)`
- `getThreadContext(threadHandle)`
- `setThreadContext(threadHandle, instructionPointer)`

### Window
- `findWindow(windowTitle)`
- `window.find(windowTitle)`
- `listWindowsByPID(pid)`
- `findWindowByPID(pid, title)`
- `sendWindowMessage(hwnd, message, wparam, lparam)`

### Injection
- `injectDLL(pid, dllPath)`
- `inject.dll(pid, dllPath)`
- `testInjectDLL(pid)` (loads `TestDLL.dll` next to the built executable)
- `inject.test(pid)`
- `injectShellcode(pid, hexBytes)` (check-mode guard implemented; execution currently blocked until byte decoding lands)
- `createRemoteThread(pid, startAddress, parameter)`

### Debug / Token
- `debugAttach(pid)`
- `debugWaitEvent()`
- `debugReadRegisters(tid)`
- `debugContinue(eventCode)`
- `openProcessToken(handle)`
- `enablePrivilege(privilegeName)`

## Build (Visual Studio)

1. Open `Wapi.slnx` (or `native/Wapi.vcxproj`) in Visual Studio.
2. Select target platform (`x64` or `ARM64`) and configuration (`Debug`/`Release`).
3. Build the main `Wapi` project.
4. Build `TestDLL` too if you want `testInjectDLL(pid)` to work.

## Running Locally

From the build output directory (typically `x64\Debug` from the solution or `native\x64\Debug` from the project):

```powershell
.\Wapi.exe run "listProcesses()"
.\Wapi.exe check "int pid = findProcessPID(\"notepad\") int h = openProcess(pid)"
.\Wapi.exe test
```

Tip: start Notepad before running `wapi test` for process-dependent checks.

## Notes

- This project uses low-level Windows APIs and can be unstable if used on invalid targets/addresses.
- `check` mode is intended for preflight/static-style verification with side effects suppressed.
- High-risk APIs are capability-gated and require `--mode dev` or `--mode unsafe` in addition to the relevant `--cap` grant; injection APIs still require `--allow-injection` outside unsafe mode. Use `--json` to emit machine-readable event lines for IDE integrations while preserving normal CLI output.

## License

MIT - see `LICENSE`.


## Wapi IDE (Tauri)

The desktop IDE uses Tauri 2 with a vanilla HTML, CSS, and JavaScript renderer. The native host preserves guarded `check` / `run` execution, project dialogs, recent projects, file persistence, custom window controls, and PTY terminal sessions.

Requirements:

- Node.js 20 or newer
- Rust stable with the MSVC toolchain
- Microsoft Edge WebView2 Runtime
- Visual Studio Build Tools with Desktop development with C++

Run the desktop app:

```powershell
npm install
npm run dev
```

Build the installer and executable:

```powershell
npm run build
```
