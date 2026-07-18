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

Reading `proc.find`, `proc.open`, and `mem.write` calls in a Windows API script doesn't tell you the blast radius. Wapi scripts declare requirements up front with `#mode` and `#cap`, then the runtime compares those declarations with explicit CLI or confirmed trusted-script grants before any Windows API call executes.

## Why Wapi

PowerShell can call into the Windows API, but the P/Invoke syntax is painful and there's no safety model. Python with `ctypes` works but is verbose, brittle, and equally ungoverned. Frida is powerful but assumes you already trust the script. Wapi is a small purpose-built language with:

- a real lexer -> parser -> evaluator pipeline, not a wrapper script,
- a capability system where scripts declare what they need (`#cap proc.read memory.write`) and the runtime enforces it per function call,
- four enforced modes - `safe`, `dev`, `unsafe`, `dangerous` - with a hard floor a script can't silently exceed,
- full audit logging of every privileged call,
- a native drag-and-drop terminal launcher with no web runtime or desktop frontend dependencies.

## Quick start

```powershell
git clone https://github.com/ethical-tyce/Wapi.git
cd Wapi

# build the CLI
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' .\native\Wapi.vcxproj /p:Configuration=Release /p:Platform=x64

# run a trusted local script that uses its directive block as the grant set
.\native\x64\Release\Wapi.exe run examples/process_check.wapi --trust-script-directives
```

Prebuilt binaries aren't published yet - see [Build native runtime](#build-native-runtime) below for full requirements. The current application is the standalone native `Wapi.exe`.

## The capability model

Every `.wapi` file can open with a directive block. Directives are read before the lexer runs, so they're not language statements. They declare what the script requires; CLI flags or an explicit trusted-script confirmation decide what is actually granted.

```wapi
#mode safe
#cap runtime.print
#cap file.write("generated_payloads/**")
#deny file.write("generated_payloads/private/**")
#strict
```

- `#mode safe|dev|unsafe|dangerous` - the minimum mode this script requires. Running it under a lower externally granted mode is a hard error; `dangerous` must be granted externally.
- `#cap <rule> [<rule>...]` - capability requirements. Bare rules such as `runtime.print` retain their broad behavior. Scoped rules are requirements, not grants, unless `--trust-script-directives` is used for a trusted script.
- `#deny <rule> [<rule>...]` - restrictions that apply even when script directives are untrusted. A matching deny always overrides every grant and is merged across included files.
- `#strict` - missing runtime capability grants become hard errors instead of warnings.
- `#allow-injection` - declares injection helper usage; `--allow-injection`, `unsafe`/`dangerous` mode, or trusted directives are still required to grant ordinary injection.
- `#include "file.wapi"` - include another `.wapi` file relative to the source root (cannot be absolute or escape the root).
- `#name`, `#version`, `#author`, `#description` - script metadata surfaced by the terminal launcher and `wapi doc`.

The first scoped-rule slice supports exact paths and terminal `/**` subtrees for the existing `file.write` and `pe.inspect` capabilities:

```wapi
#cap pe.inspect("build/plugin.dll")
#cap file.write("generated_payloads/**")
#deny file.write("generated_payloads/private/**")
```

Relative scopes resolve from the launch working directory. Wapi authorizes the lexical target first, resolves the nearest existing path, then authorizes the resolved target again before using it. Ordinary `*`, `?`, mid-pattern wildcards, wildcard grants, and resource scopes on other capabilities are rejected. This is target-aware authorization for these two bindings, not yet a complete filesystem sandbox for DLL loading or other path-consuming APIs.

Run someone else's script with a tighter external rule set than it declares and the runtime - not the script - decides what's actually granted:

```powershell
wapi check untrusted.wapi --strict-permissions `
  --cap 'file.write("generated_payloads/**")' `
  --deny 'file.write("generated_payloads/private/**")'
```

`--cap` and `--deny` are repeatable. Capability names are matched case-insensitively. Exact denies, namespace denies such as `inject.*`, and the global deny `*` are supported; wildcard allows are deliberately rejected.

For trusted local scripts, `--trust-script-directives` promotes ordinary `#mode`, `#cap`, and `#allow-injection` declarations into runtime grants. It deliberately cannot grant `dangerous` mode or `inject.manualmap`; those must come from explicit CLI flags. Do not use trusted directives for scripts you would not already trust to run with those permissions.

`wapi lint` compares function calls with the base capability of each `#cap` rule, so a scoped `file.write(...)` declaration satisfies the `file.write` declaration check while runtime enforcement still validates the resolved target.

## Project structure

| Path | What's there |
|---|---|
| `native/src/` | C++ CLI, lexer, parser, evaluator, Windows API runtime |
| `native/TestDLL/` | Sample DLL used by `testInjectDLL(pid)` |
| `native/Wapi.vcxproj` | Main Visual Studio project |
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

Every function below has a global name and a dotted alias, and is gated behind a runtime capability grant. Scripts should declare matching requirements with `#cap`; operators grant them with `--cap` or trusted directives.

**Process** - `findProcessPID` / `proc.find`, `openProcess` / `proc.open`, `terminateProcess` / `proc.terminate`, `suspendProcess` / `proc.suspend`, `resumeProcess` / `proc.resume`, `closeProcess` / `proc.close`, `closeHandle` / `handle.close`

**Modules** - `listModules` / `proc.modules`, `getModuleBase`, `getModuleBaseAddress` / `proc.module.base`, `getModuleSize` / `proc.module.size`

**Memory** - `readMemory` / `mem.read`, `scanPattern` / `mem.scan`, `writeMemory` / `mem.write`, `allocMemory` / `mem.alloc`, `freeMemory` / `mem.free`, `protectMemory` / `mem.protect`, `queryMemory` / `mem.query`, `listMemoryRegions` / `mem.regions`, and `listExecutableRegions` / `mem.executableRegions`
Typed memory access is available through `mem.readInt32`, `mem.readInt64`, `mem.readFloat`, `mem.readDouble`, and `mem.readPtr`, with matching write functions. `mem.follow` resolves bounded pointer chains.

Small and unsigned integers use `mem.readInt8`, `mem.readUInt8`, `mem.readInt16`, `mem.readUInt16`, `mem.readUInt32`, and `mem.readUInt64`, with matching writes. Buffer and text helpers are `mem.readBytes`, `mem.writeBytes`, `mem.readString`, and `mem.writeString`. Validate uncertain ranges with `mem.isReadable` and `mem.isWritable`.


**Detection / inspection** - `detect.unbackedExecutable(pid)` returns suspicious executable regions, `detect.peImage(handle, base)` validates an in-memory PE image, and `pe.inspect(path)` inspects an on-disk EXE or DLL without loading it.

**Threads** - `listThreads` / `thread.list`, `openThread` / `thread.open`, `suspendThread` / `thread.suspend`, `resumeThread` / `thread.resume`, `getThreadContext` / `thread.context`, `setThreadContext` / `thread.context.set`, `getThreadStartAddress` / `thread.startAddress`

**Window** - `findWindow` / `window.find`, `listWindowsByPID` / `window.listByPid`, `findWindowByPID` / `window.findByPid`, `sendWindowMessage` / `window.message`

**Injection** - standard loading uses `injectDLL` / `inject.dll` (also `inject.loadLibrary`) as `inject.dll(pid, "relative/or/absolute/plugin.dll")`. It validates that the path is an existing native DLL, checks target architecture, and returns the loaded module base. `testInjectDLL` / `inject.test` uses the fixture DLL. Shellcode and remote-thread helpers remain `injectShellcode` / `inject.shellcode` and `createRemoteThread` / `inject.thread`. Ordinary injection needs `--allow-injection` below `unsafe` mode.

Source scaffolding is separate from loading. `inject.writePayloadSRC(language, source)` writes C, C++, Rust, or Zig source under `generated_payloads/` and returns its path. `payload.writeSource(language, relativePath, source)` selects the relative path. Build that source with its normal toolchain, inspect the resulting DLL with `pe.inspect`, then pass the compiled `.dll` path to `inject.dll`. Wapi does not guess a compiled DLL's original source language and does not compile-and-inject in one call.

Manual mapping uses `manualMapDLL` / `inject.manualMap`; `inject.manualMapReport` returns structured validation/mapping details. It always requires externally granted `dangerous` mode and the explicit `inject.manualmap` capability. The bounded first version is x64-only and deliberately does not erase PE headers, unlink loader records, hide threads, or bypass security products. It refuses critical processes and targets with CFG or enforced dynamic-code, binary-signature, or image-load mitigation policies.

Both DLL loaders are source-language independent for native PE DLLs: C, C++, Rust, Zig, and similar toolchains work when Wapi, the target, and DLL architecture match. Managed C# assemblies and Python, Java, or JavaScript payloads require a dedicated runtime/bootstrap and are not treated as native DLLs.

The bounded manual mapper rejects TLS callbacks/static TLS, CLR images, delay imports, and writable-executable sections. Imported dependencies must already be loaded or discoverable through the target process's normal DLL search path; the mapped DLL's directory is not automatically added.

**Debug / token** - `debugAttach` / `debug.attach`, `debugWaitEvent` / `debug.wait`, `debugReadRegisters` / `debug.registers`, `debugContinue` / `debug.continue`, `openProcessToken` / `token.open`, `enablePrivilege` / `token.privilege`

## CLI

```
wapi run <file>          run a script
wapi check <file>        preflight check, side effects suppressed
wapi lint <file>         capability cross-check + static analysis
wapi fmt <file>          format in place
wapi bundle <file>       inline includes and preserve merged policy directives
wapi doc <file>          print script metadata + capability summary
wapi init                scaffold a new project
wapi repl                interactive shell
```

Add `--json` to any command for machine-readable event lines.

## Terminal launcher

Run `Wapi.exe` without arguments to open the branded terminal. Drag a `.wapi` file into the window and press Enter, or drag the script directly onto `Wapi.exe`.

- the terminal displays the script's requested mode and capabilities before execution
- Run explicitly trusts ordinary declarations for that local execution
- Check performs a side-effect-free preflight
- dangerous mode and `inject.manualmap` remain unavailable from drag-and-drop and require explicit CLI flags

The old Tauri/Vite frontend is intentionally removed for now. Wapi only requires the native Visual C++ toolchain.

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
- Prefer directive blocks in committed scripts so required capabilities and mode are visible, but grant them explicitly with CLI flags or `--trust-script-directives` for trusted local scripts.
- Use `--json` for machine-readable output when integrating with other tools.

## License

MIT - see [`LICENSE`](LICENSE).
