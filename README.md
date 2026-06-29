# Wapi

Wapi is a C++ command-line language and desktop IDE for controlled Windows API scripting.

It is built around a small custom language, explicit file-level directives, and a safety-first runtime policy model. Scripts can inspect processes, inspect modules, work with process memory, find windows, and call higher-risk runtime helpers only when the script and runtime policy both allow it.

## Current Status

This repository is an early Wapi build with:

- a native lexer/parser/evaluator pipeline in `native/src/`,
- a directive-aware CLI for `run`, `check`, `lint`, `fmt`, `bundle`, `doc`, and project scaffolding,
- a Tauri/Vite IDE with Monaco editing, project templates, diagnostics, settings, and terminal integration,
- implemented process, memory, module, thread, window, injection, debug, and token runtime bindings,
- runtime audit output and strict capability checks.

## Project Structure

- `native/src/` - C++ CLI, lexer, parser, evaluator, and Windows API runtime.
- `native/TestDLL/` - sample DLL project used by `testInjectDLL(pid)`.
- `native/Wapi.vcxproj` - main Visual Studio project.
- `src/` - Vite renderer for the desktop IDE.
- `src-tauri/` - Tauri host, guarded runtime execution, projects, and terminal integration.
- `docs/language.md` - current Wapi language and directive reference.
- `docs/assets/` - application branding shared by the renderer and desktop bundle.
- `docs/design/` - design reference and visual QA notes.
- `Wapi.slnx` - root Visual Studio solution for the native projects.

## Script Shape

New scripts should start with a directive block. Directives are read before lexing, so they are not normal language statements.

```wapi
#name "Process quick check"
#version "1.0.0"
#mode safe
#strict
#cap proc.list runtime.print

let target = "notepad"
var pid = proc.find(target)
print("{target} pid={pid}")
```

Common directives:

- `#mode safe|dev|unsafe` sets the script mode requirement.
- `#cap <name> [<name>...]` declares runtime capabilities used by the file.
- `#include "relative/file.wapi"` includes another file before parsing.
- `#strict` makes missing capabilities hard errors.
- `#allow-injection` allows injection helpers outside `unsafe` mode.
- `#name`, `#version`, `#author`, and `#description` add script metadata.

Old inline `include "helpers.wapi"` still works, but `#include` is preferred for file-level dependencies.

## Language Snapshot

The language currently supports:

- `var`, `let`, and `const` declarations with inference,
- typed declarations: `int`, `long`, `string`, `bool`, `double`, `float`, and custom struct names,
- string, int, hex, bool, double, and `null` literals,
- arrays, indexing, method calls, and field access,
- string interpolation with `{expr}` inside strings,
- arithmetic, comparison, logical, bitwise, compound assignment, increment, decrement, ternary, and null-coalescing expressions,
- `if` / `else`, `while`, and `for i in range(...)` loops,
- `func` declarations with `->` return annotations,
- `struct` declarations and struct literals,
- `match` with literal, binding, guarded, and `_` default arms,
- `try` / `catch`,
- named arguments in calls, for example `add(right: 2, left: 1)`,
- null-safe method calls with `?.` and fallback with `??`,
- dotted runtime aliases such as `proc.find`, `proc.modules`, `mem.read`, and `window.find`.

Language-only example:

```wapi
#name "Language smoke"
#mode safe
#strict
#cap runtime.print language.core language.array language.math

struct Point {
    int x
    int y
}

func describe(string name, int value) -> string {
    return "{name}={value}"
}

var values = []
values.push(10)
values.push(20)

Point p = Point { x: values.len(), y: max(3, 7) }
var maybe = null
var fallback = maybe?.len() ?? p.y

match fallback {
    7 => print(describe("fallback", fallback))
    _ => print("unexpected {fallback}")
}
```

## CLI Usage

```txt
wapi run <script-or-file.wapi|-> [options]
wapi check <script-or-file.wapi|-> [options]
wapi lint|validate <script-or-file.wapi|-> [options]
wapi fmt <script-or-file.wapi> [--write]
wapi bundle <file.wapi>... [-o output.wapi]
wapi init [directory]
wapi doc [function|syntax|directives]
wapi completions [powershell|bash]
wapi test
```

Useful options:

- `--mode safe|dev|unsafe`
- `--cap <capability>` repeated as needed
- `--strict-permissions`
- `--allow-injection`
- `--timeout <ms>`
- `--max-steps <n>`
- `--json`
- `--quiet`
- `--verbose`
- `--trace`
- `--profile`
- `--env KEY=VALUE`

File directives are the normal way to describe a script. CLI flags are still useful for wrappers, CI, and one-off overrides. If an explicit CLI mode is lower than the script's `#mode`, Wapi fails before evaluation.

Examples:

```powershell
.\native\x64\Debug\Wapi.exe lint .\examples\language.wapi --mode safe
.\native\x64\Debug\Wapi.exe check .\examples\language.wapi --mode safe --strict-permissions
.\native\x64\Debug\Wapi.exe run .\examples\language.wapi
.\native\x64\Debug\Wapi.exe doc directives
.\native\x64\Debug\Wapi.exe fmt .\examples\language.wapi --write
```

For stdin:

```powershell
"#mode safe`n#cap runtime.print`nprint(1)" | .\native\x64\Debug\Wapi.exe run -
```

## Safety Model

Wapi enforces policy at runtime with:

- script directives: `#mode`, `#cap`, `#strict`, `#allow-injection`,
- CLI flags: `--mode`, `--cap`, `--strict-permissions`, `--allow-injection`,
- evaluator-side checks for dev-only and injection-only capabilities.

In compatibility mode, missing capabilities emit warnings and audit logs. In strict mode, missing capabilities become hard errors.

Audit lines are emitted as `[WAPI_AUDIT] ...`. `lint` emits `[LINT] ...` lines for missing or unused `#cap` declarations without executing the script.

## Implemented Functions

### Language Helpers

- `len(value)`
- `substr(text, start, count)`
- `contains(text, needle)`
- `replace(text, needle, replacement)`
- `toLower(text)`
- `toInt(value)`
- `abs(number)`
- `min(a, b)`
- `max(a, b)`
- `push(array, value)`
- `pop(array)`
- `typeof(value)`
- `assert(condition, message)`
- `toHex(number)`
- `fromHex(text)`
- `split(text, delimiter)`
- `trim(text)`
- `padLeft(text, width, fill)`
- `padRight(text, width, fill)`
- `sort(array)`

Many helpers also have method form, such as `text.len()`, `text.contains(value)`, `text.trim()`, `items.push(value)`, `items.pop()`, and `items.sort()`.

### Runtime

- `print(value)`

### Process

- `listProcesses()` / `proc.list()`
- `findProcessPID(name)` / `proc.find(name)`
- `openProcess(pid)` / `proc.open(pid)`
- `terminateProcess(handle)` / `proc.terminate(handle)`
- `suspendProcess(handle)` / `proc.suspend(handle)`
- `resumeProcess(handle)` / `proc.resume(handle)`
- `closeProcess(handle)` / `proc.close(handle)`
- `closeHandle(handle)` / `handle.close(handle)`

### Modules

- `listModules(pid)` / `proc.modules(pid)`
- `getModuleBase(pid, moduleName)`
- `getModuleBaseAddress(pid, moduleName)` / `proc.module.base(pid, moduleName)`
- `getModuleSize(pid, moduleName)` / `proc.module.size(pid, moduleName)`

### Memory

- `readMemory(handle, address)` / `mem.read(handle, address)`
- `writeMemory(handle, address, value)` / `mem.write(handle, address, value)`
- `allocMemory(handle, size)` / `mem.alloc(handle, size)`
- `freeMemory(handle, address)` / `mem.free(handle, address)`
- `protectMemory(handle, address, size, protection)` / `mem.protect(handle, address, size, protection)`
- `queryMemory(handle, address)` / `mem.query(handle, address)`

### Threads

- `listThreads(pid)` / `thread.list(pid)`
- `openThread(tid)` / `thread.open(tid)`
- `suspendThread(threadHandle)` / `thread.suspend(threadHandle)`
- `resumeThread(threadHandle)` / `thread.resume(threadHandle)`
- `getThreadContext(threadHandle)` / `thread.context(threadHandle)`
- `setThreadContext(threadHandle, instructionPointer)` / `thread.context.set(threadHandle, instructionPointer)`

### Window

- `findWindow(windowTitle)` / `window.find(windowTitle)`
- `listWindowsByPID(pid)` / `window.listByPid(pid)`
- `findWindowByPID(pid, title)` / `window.findByPid(pid, title)`
- `sendWindowMessage(hwnd, message, wparam, lparam)` / `window.message(hwnd, message, wparam, lparam)`

### Injection

- `injectDLL(pid, dllPath)` / `inject.dll(pid, dllPath)`
- `testInjectDLL(pid)` / `inject.test(pid)` / `inject.testDll(pid)`
- `injectShellcode(pid, hexBytes)` / `inject.shellcode(pid, hexBytes)`
- `createRemoteThread(pid, startAddress, parameter)` / `inject.thread(pid, startAddress, parameter)`

Injection helpers require `#allow-injection` outside `unsafe` mode.

### Debug / Token

- `debugAttach(pid)` / `debug.attach(pid)`
- `debugWaitEvent()` / `debug.wait()`
- `debugReadRegisters(tid)` / `debug.registers(tid)`
- `debugContinue(eventCode)` / `debug.continue(eventCode)`
- `openProcessToken(handle)` / `token.open(handle)`
- `enablePrivilege(privilegeName)` / `token.privilege(privilegeName)`

## IDE

The desktop IDE uses Tauri 2 with a vanilla HTML, CSS, and JavaScript renderer.

The IDE provides:

- directive-aware Monaco syntax highlighting, completion, hover, and quick fixes,
- directive-first project templates,
- Script Info in Settings for `#mode`, metadata, includes, and `#cap` directives,
- `lint`-based diagnostics while editing,
- guarded native `check` / `run` execution through the Tauri host,
- project dialogs, recent projects, file persistence, custom window controls, and PTY terminal sessions.

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

Build the web renderer only:

```powershell
npm run build:web
```

Build the installer and executable:

```powershell
npm run build
```

## Build Native Runtime

1. Open `Wapi.slnx` or `native/Wapi.vcxproj` in Visual Studio.
2. Select target platform (`x64` or `ARM64`) and configuration (`Debug` or `Release`).
3. Build the main `Wapi` project.
4. Build `TestDLL` too if you want `testInjectDLL(pid)` to work.

From the repository root with Visual Studio installed:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' .\native\Wapi.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
```

## Notes

- This project uses low-level Windows APIs and can be unstable if used on invalid targets or addresses.
- `check` mode is intended for preflight verification with side effects suppressed.
- Prefer directive blocks in committed scripts so capabilities and mode are visible in the file itself.
- Use `--json` for machine-readable event lines when integrating with tools.

## License

MIT - see `LICENSE`.
