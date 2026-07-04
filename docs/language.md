# Wapi Language Reference

This document tracks the language shape implemented by the current native lexer, parser, evaluator, CLI preprocessor, and IDE helpers.

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

- `#mode safe|dev|unsafe` declares the minimum runtime mode.
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

var pid = proc.find("notepad")
let handle = proc.open(pid)
let address = mem.alloc(handle, 64)
mem.write(handle, address, 1234)
print(mem.read(handle, address))
mem.free(handle, address)
handle.close(handle)
```

Injection helpers additionally need an injection grant outside `unsafe` mode. Use `--allow-injection`, IDE project settings, or `--trust-script-directives` for trusted scripts that declare `#allow-injection`.

## CLI Flow

Use this order when checking scripts manually:

```powershell
wapi lint script.wapi --mode safe
wapi check script.wapi --mode safe --strict-permissions --cap runtime.print
wapi run script.wapi --trust-script-directives
```

`lint` reads directives, parses the program, emits `[LINT]` warnings for missing or unused `#cap` declarations, and exits without running the evaluator.
