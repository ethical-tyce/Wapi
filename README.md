# Wapi

Wapi is a C++ command-line language/runtime for controlled Windows API scripting.

It gives you a small custom language (`int` variables + function calls) that can:
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
- string/int/hex literals,
- function calls as statements and expressions.

Example script:

```txt
int pid = findProcessPID("notepad")
int handle = openProcess(pid)
suspendProcess(handle)
resumeProcess(handle)
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
- `findProcessPID(name)`
- `openProcess(pid)`
- `terminateProcess(handle)`
- `suspendProcess(handle)`
- `resumeProcess(handle)`

### Memory
- `readMemory(handle, address)`
- `writeMemory(handle, address, value)`
- `allocMemory(handle, size)`
- `freeMemory(handle, address)`

### Window
- `findWindow(windowTitle)`

### Injection
- `injectDLL(pid, dllPath)`
- `testInjectDLL(pid)` (loads `TestDLL.dll` next to the built executable)

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