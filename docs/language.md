# Wapi Language Reference

This document tracks the language shape implemented by the native lexer, parser, evaluator, and CLI preprocessor.

## File Directives

Directives are parsed before lexing. Put them at the top of a file before normal code.

```wapi
#name "Example"
#version "1.0.0"
#author "ethical-tyce"
#description "Small safe-mode script"
#mode safe
#strict
#cap runtime.print proc.list
#include "helpers.wapi"

print("ready")
```

Supported directives:

- `#mode safe|dev|unsafe|dangerous` declares the minimum runtime mode. `dangerous` must be granted externally.
- `#cap <capability> [<capability>...]` declares required capabilities. It does not grant them unless the CLI is run with `--trust-script-directives`.
- `#include "relative/path.wapi"`
- `#strict`
- `#allow-injection` declares injection helper usage. It is not an injection grant unless directives are trusted.
- `#name "..."`
- `#version "..."`
- `#author "..."`
- `#description "..."`

`#include` is preferred. Old body-level `include "file.wapi"` still works for compatibility.

## Declarations

```wapi
var inferred = 10
let fixed = "cannot reassign"
const answer = 42
const int typed = 7
int count = 0
long address = 0x1000
string target = "notepad"
bool enabled = true
double ratio = 0.5
float other = 1.25
```

`let` and `const` reject reassignment. Semicolons are optional.

## Expressions

Wapi supports:

- `+ - * / %`
- `== != < <= > >=`
- `&& || !`
- `& | ^ ~ << >>`
- `+= -= *= /= %=`
- `++ --`
- `condition ? whenTrue : whenFalse`
- `left ?? fallback`
- `target?.method()`
- array indexing with `items[index]`
- named arguments such as `add(right: 2, left: 1)`

## Strings

Strings support escapes and interpolation.

```wapi
let target = "notepad"
var pid = 1234
print("{target} pid={pid}")
print("line one\nline two")
```

## Arrays And Methods

```wapi
var values = []
values.push(10)
values.push(20)
print(values.len())
print(values[0])
print(values.pop())
```

Supported method forms include:

- `text.len()` / `items.len()`
- `text.size()` / `items.size()`
- `text.trim()`
- `text.toLower()` / `text.toUpper()`
- `text.contains(value)` / `items.contains(value)`
- `text.startsWith(value)` / `text.endsWith(value)`
- `items.push(value)`
- `items.pop()`
- `items.first()` / `items.last()`
- `items.sort()`

## Control Flow

```wapi
if enabled {
    print("enabled")
} else {
    print("disabled")
}

var i = 0
while i < 3 {
    print("i={i}")
    i++
}

for n in range(0, 10, 2) {
    print(n)
}
```

## Functions

```wapi
func add(int left, int right) -> int {
    return left + right
}

print(add(right: 2, left: 1))
```

The return type defaults to `auto` if omitted.

## Structs

```wapi
struct Point {
    int x
    int y
}

Point p = Point { x: 2, y: 5 }
p.x += 1
print("{p.x},{p.y}")
```

## Match

```wapi
match value {
    0 => print("zero")
    other if other > 10 => print("large {other}")
    _ => print("other")
}
```

## Errors

```wapi
try {
    assert(false, "bad state")
} catch err {
    print("caught {err}")
}
```

## Runtime Calls

Use dotted aliases for new examples:

```wapi
#mode safe
#strict
#cap proc.list proc.modules runtime.print

let target = "notepad"
var pid = proc.find(target)
print("pid={pid}")
proc.modules(pid)
```

Higher-risk work should make the policy explicit in the file:

```wapi
#mode dev
#strict
#cap proc.list proc.open.all_access mem.alloc mem.write mem.read mem.free proc.handle.close runtime.print
#cap proc.modules

var pid = proc.find("notepad")
let handle = proc.open(pid)
let address = mem.alloc(handle, 64)
mem.write(handle, address, 1234)
print(mem.read(handle, address))
mem.free(handle, address)
handle.close(handle)
```
Base-address offsets use normal arithmetic:

```wapi
let base = proc.module.base(pid, "notepad.exe")
let value = mem.readInt32(handle, base + 0x1234)

// Pointer width automatically matches the target process.
let player = mem.readPtr(handle, base + 0x5678)
mem.writeFloat(handle, player + 0x20, 100.0)

// Equivalent to: [[base + 0x5678] + 0x20] + 0x100
let healthAddress = mem.follow(handle, base + 0x5678, [0x20, 0x100])
print(mem.readFloat(handle, healthAddress))
```

`mem.read` and `mem.write` remain signed 32-bit integer operations for compatibility. Their explicit aliases are `mem.readInt32` and `mem.writeInt32`. Additional typed pairs are `Int64`, `Float`, `Double`, and `Ptr`.

`mem.follow` accepts 1 to 32 signed integer offsets. It rejects null pointers and address overflow, uses the target process's pointer width, and requires `mem.read`. All typed writes require `mem.write`.

`mem.readBytes` returns an integer array and `mem.writeBytes` accepts values from 0 through 255. `mem.readString(handle, address, maxLength, encoding)` supports `ascii`, `utf8`, and `utf16le`; `mem.writeString` uses the same encodings and writes a null terminator. `mem.isReadable` and `mem.isWritable` validate a complete range across region boundaries.

Detector-oriented inspection is available without modifying the target: `mem.regions(handle)` lists committed regions, `mem.executableRegions(handle)` filters executable regions, `detect.unbackedExecutable(pid)` highlights executable private memory, `detect.peImage(handle, base)` parses a possible in-memory PE image, and `thread.startAddress(threadHandle)` attributes a thread start address to its module and memory region. Unregistered `MEM_IMAGE` regions remain classified in the general region list but are not automatically treated as malicious. On-disk files can be checked with `pe.inspect(path)`.

Payload source and compiled DLLs are deliberately separate:

```wapi
#mode dev
#strict
#cap proc.list file.write pe.inspect inject.dll
#allow-injection
let pid = proc.find("notepad")

let sourcePath = inject.writePayloadSRC("cpp", "int exported_marker = 1;")
// Build sourcePath with your C++ toolchain. The result must be a native PE DLL.
let dllInfo = pe.inspect("build/plugin.dll")
let moduleBase = inject.dll(pid, "build/plugin.dll")
```

`inject.writePayloadSRC(language, source)` accepts `c`, `cpp`/`c++`, `rust`, or `zig`, writes under `generated_payloads/`, and returns the generated relative path. `payload.writeSource(language, relativePath, source)` provides an explicit workspace-relative path with the correct language extension. Source writing requires `file.write`; it never invokes a compiler. `inject.dll(pid, dllPath)` requires an existing `.dll`, validates its PE type and target architecture, loads it through the target's normal Windows loader, and returns the remote module base.



Ordinary injection helpers additionally need an injection grant below `unsafe` mode. Use `--allow-injection` or `--trust-script-directives` for trusted scripts that declare `#allow-injection`.

`manualMapDLL` / `inject.manualMap` is separate; `inject.manualMapReport(pid, dllPath)` returns a struct with validation, image, import, relocation, unwind, entry-point, protection, and remote-base details. Both require externally granted `dangerous` mode and the explicit `inject.manualmap` capability. `--trust-script-directives` cannot grant either boundary. The bounded mapper accepts a validated subset of x64 native PE DLLs, resolves relocations and imports, invokes `DllMain`, registers validated x64 unwind data, and applies final non-RWX section protections. It rejects CLR/mixed-mode images, delay imports, TLS callbacks/static TLS, CFG images/targets, writable-executable sections, critical Windows targets, and targets with enforced dynamic-code, signature, or image-load policies.

Standard and manual-map loading are source-language independent for native DLLs produced by C, C++, Rust, Zig, and similar toolchains. Managed C#, Python, Java, and JavaScript payloads need an explicit runtime/bootstrap and are not loaded as native DLLs.

Manual-map dependencies must already be loaded or discoverable through the target's normal DLL search path. Wapi does not add the selected DLL's directory to that search path.

## CLI Flow

Use this order when checking scripts manually:

```powershell
wapi lint script.wapi --mode safe
wapi check script.wapi --mode safe --strict-permissions --cap runtime.print
wapi run script.wapi --trust-script-directives
```

`lint` reads directives, parses the program, emits `[LINT]` warnings for missing or unused `#cap` declarations, and exits without running the evaluator.
