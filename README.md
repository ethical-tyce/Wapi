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

- `Wapi.cpp` - CLI entrypoint, command parsing, test harness.
- `lexer.cpp/.h` - tokenization.
- `parser.cpp/.h` - AST parsing.
- `evaluator.cpp/.h` - runtime evaluation + Windows API bindings + policy enforcement.
- `TestDLL/` - sample DLL project used by `testInjectDLL(pid)`.
- `Wapi.vcxproj` - main Visual Studio project.

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

### Runtime
- `print(value)`

### Window
- `findWindow(windowTitle)`
- `window.find(windowTitle)`

### Injection
- `injectDLL(pid, dllPath)`
- `inject.dll(pid, dllPath)`
- `testInjectDLL(pid)` (loads `TestDLL.dll` next to the built executable)
- `inject.test(pid)`

## Build (Visual Studio)

1. Open `Wapi.vcxproj` (or solution) in Visual Studio.
2. Select target platform (`x64` or `ARM64`) and configuration (`Debug`/`Release`).
3. Build the main `Wapi` project.
4. Build `TestDLL` too if you want `testInjectDLL(pid)` to work.

## Running Locally

From the build output directory (for example `x64\Debug`):

```powershell
.\Wapi.exe run "listProcesses()"
.\Wapi.exe check "int pid = findProcessPID(\"notepad\") int h = openProcess(pid)"
.\Wapi.exe test
```

Tip: start Notepad before running `wapi test` for process-dependent checks.

## Notes

- This project uses low-level Windows APIs and can be unstable if used on invalid targets/addresses.
- `check` mode is intended for preflight/static-style verification with side effects suppressed.
- Several APIs shown by `wapi test` are roadmap placeholders and are expected to fail until implemented.

## License

MIT - see `LICENSE`.
