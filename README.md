# Wapi

**A scripting language for Windows API automation where every script declares required capabilities before it runs.**

```wapi
#name "Process quick check"
#mode safe
#strict
#cap proc.list runtime.print

let target = "notepad"
var pid = proc.find(target)
print("{target} pid={pid}")
```

Reading `proc.find`, `proc.open`, and `mem.write` calls in a Windows API script doesn't tell you the blast radius. Wapi scripts declare requirements up front with `#mode` and `#cap`, then the runtime compares those declarations with explicit CLI, trusted-script, or IDE project grants before any Windows API call executes.

## Why Wapi

PowerShell can call into the Windows API, but the P/Invoke syntax is painful and there's no safety model. Python with `ctypes` works but is verbose, brittle, and equally ungoverned. Frida is powerful but assumes you already trust the script. Wapi is a small purpose-built language with:

- a real lexer -> parser -> evaluator pipeline, not a wrapper script,
- a capability system where scripts declare what they need (`#cap proc.read memory.write`) and the runtime enforces it per function call,
- three enforced modes - `safe`, `dev`, `unsafe` - with a hard floor a script can't silently exceed,
- full audit logging of every privileged call,
- a desktop IDE with Monaco editing, inline diagnostics, a process explorer, and a live capability inspector.

## Quick start

```powershell
git clone https://github.com/ethical-tyce/Wapi.git
cd Wapi

# build the CLI
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' .\native\Wapi.vcxproj /p:Configuration=Release /p:Platform=x64

# run a trusted local script that uses its directive block as the grant set
.\native\x64\Release\Wapi.exe run examples/process_check.wapi --trust-script-directives
```

Prebuilt binaries aren't published yet - see [Build native runtime](#build-native-runtime) below for full requirements. A tagged release with a standalone `Wapi.exe` and IDE installer is on the roadmap.

## The capability model

Every `.wapi` file can open with a directive block. Directives are read before the lexer runs, so they're not language statements. They declare what the script requires; CLI flags, trusted-script mode, or IDE project settings decide what is actually granted.

```wapi
#mode dev
#cap proc.read proc.write
#cap memory.read memory.write
#strict
```

- `#mode safe|dev|unsafe` - the minimum mode this script requires. Running it under a lower externally granted mode is a hard error.
- `#cap <name> [<name>...]` - capabilities this script uses. These are requirements, not grants, unless you explicitly pass `--trust-script-directives` for a trusted script.
- `#strict` - missing runtime capability grants become hard errors instead of warnings.
- `#allow-injection` - declares injection helper usage; `--allow-injection`, `unsafe` mode, or trusted directives are still required to grant it.
- `#include "file.wapi"` - include another `.wapi` file relative to the source root (cannot be absolute or escape the root).
- `#name`, `#version`, `#author`, `#description` - script metadata, surfaced in the IDE and `wapi doc`.

Run someone else's script with a tighter cap set than it declares and the runtime - not the script - decides what's actually granted:

```powershell
wapi run untrusted.wapi --mode safe --cap proc.list --strict-permissions
```

For trusted local scripts, `--trust-script-directives` intentionally promotes `#mode`, `#cap`, and `#allow-injection` into runtime grants. Do not use it for scripts you would not already trust to run with those permissions.

`wapi lint` cross-checks every function call against your `#cap` block and tells you what's declared-but-unused and what's used-but-undeclared.

## Project structure

| Path | What's there |
|---|---|
| `native/src/` | C++ CLI, lexer, parser, evaluator, Windows API runtime |
| `native/TestDLL/` | Sample DLL used by `testInjectDLL(pid)` |
| `native/Wapi.vcxproj` | Main Visual Studio project |
| `src/` | Vite renderer for the desktop IDE |
| `src-tauri/` | Tauri host, guarded execution, projects, terminal integration |
| `docs/language.md` | Full language and directive reference |
| `Wapi.slnx` | Root Visual Studio solution |

## Language

Wapi currently supports:

- `var` / `let` / `const` with type inference, plus explicit `int`, `long`, `string`, `bool`, `double`, `float`, and struct types
- string interpolation with `{expr}`
- arrays, indexing, method calls, field access
- arithmetic, comparison, logical, bitwise, compound assignment, increment/decrement, ternary, and null-coalescing (`??`) expressions
- `if`/`else`, `while`, `for i in range(...)`
- `func` declarations with `->` return type annotations
- `struct` declarations and struct literals
- `match` with literal, binding, guarded, and `_` default arms
- `try`/`catch`
- named arguments - `add(right: 2, left: 1)`
- null-safe method calls (`?.`)
- dotted runtime aliases - `proc.find`, `mem.read`, `window.find`, etc.

```wapi
#name "Language smoke test"
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
    0 => print("zero")
    _ => print(describe("fallback", fallback))
}
```

Full syntax reference: [`docs/language.md`](docs/language.md).

## Runtime bindings

Every function below has a global name and a dotted alias, and is gated behind a runtime capability grant. Scripts should declare matching requirements with `#cap`; operators grant them with `--cap`, trusted directives, or IDE project settings.

**Process** - `findProcessPID` / `proc.find`, `openProcess` / `proc.open`, `terminateProcess` / `proc.terminate`, `suspendProcess` / `proc.suspend`, `resumeProcess` / `proc.resume`, `closeProcess` / `proc.close`, `closeHandle` / `handle.close`

**Modules** - `listModules` / `proc.modules`, `getModuleBase`, `getModuleBaseAddress` / `proc.module.base`, `getModuleSize` / `proc.module.size`

**Memory** - `readMemory` / `mem.read`, `writeMemory` / `mem.write`, `allocMemory` / `mem.alloc`, `freeMemory` / `mem.free`, `protectMemory` / `mem.protect`, `queryMemory` / `mem.query`

**Threads** - `listThreads` / `thread.list`, `openThread` / `thread.open`, `suspendThread` / `thread.suspend`, `resumeThread` / `thread.resume`, `getThreadContext` / `thread.context`, `setThreadContext` / `thread.context.set`

**Window** - `findWindow` / `window.find`, `listWindowsByPID` / `window.listByPid`, `findWindowByPID` / `window.findByPid`, `sendWindowMessage` / `window.message`

**Injection** *(requires an injection grant outside `unsafe` mode)* - `injectDLL` / `inject.dll`, `testInjectDLL` / `inject.test`, `injectShellcode` / `inject.shellcode`, `createRemoteThread` / `inject.thread`

**Debug / token** - `debugAttach` / `debug.attach`, `debugWaitEvent` / `debug.wait`, `debugReadRegisters` / `debug.registers`, `debugContinue` / `debug.continue`, `openProcessToken` / `token.open`, `enablePrivilege` / `token.privilege`

## CLI

```
wapi run <file>          run a script
wapi check <file>        preflight check, side effects suppressed
wapi lint <file>         capability cross-check + static analysis
wapi fmt <file>          format in place
wapi bundle <file>       inline all #include dependencies
wapi doc <file>          print script metadata + capability summary
wapi init                scaffold a new project
wapi repl                interactive shell
```

Add `--json` to any command for machine-readable event lines.

## IDE

Desktop app built on Tauri 2 with a vanilla HTML/CSS/JS renderer.

- directive-aware Monaco syntax highlighting, completion, hover, and quick fixes
- directive-first project templates
- Script Info panel for `#mode`, metadata, includes, and `#cap` directives
- live `lint` diagnostics while editing
- guarded native `check`/`run` execution through the Tauri host
- process explorer, execution history, variable watch, and audit log viewer
- project dialogs, recent projects, custom window controls, integrated PTY terminal

**Requirements:** Node.js 20+, Rust stable with the MSVC toolchain, Microsoft Edge WebView2 Runtime, Visual Studio Build Tools with Desktop development with C++.

```powershell
npm install
npm run dev          # run the desktop app
npm run build:web    # build the web renderer only
npm run build         # build the installer + executable
```

## Build native runtime

1. Open `Wapi.slnx` or `native/Wapi.vcxproj` in Visual Studio.
2. Select platform (`x64` or `ARM64`) and configuration (`Debug` or `Release`).
3. Build the main `Wapi` project.
4. Build `TestDLL` too if you want `testInjectDLL(pid)` to work.

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' .\native\Wapi.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
```

## Notes

- This project uses low-level Windows APIs and can be unstable against invalid targets or addresses.
- `check` mode is for preflight verification with side effects suppressed.
- Prefer directive blocks in committed scripts so required capabilities and mode are visible, but grant them explicitly with CLI flags, IDE project settings, or `--trust-script-directives` for trusted local scripts.
- Use `--json` for machine-readable output when integrating with other tools.

## License

MIT - see [`LICENSE`](LICENSE).
