# Axon

A compiled systems language with Rust's rules, C#'s syntax and C's speed.
The compiler (`axonc`, written in C++) turns Axon into C (or, with its own backends, into
x86-64 assembly or LLVM IR) and builds a native executable.

```csharp
interface Shape {
    double area();
}

struct Circle : Shape {
    double r;
    double area() { return 3.14159 * r * r; }
}

struct Stack<T> {
    List<T> items;
    mut void push(T item) { items.add(item); }
}

int main() {
    List<Box<Shape>> shapes = List<Box<Shape>>();
    shapes.add(Box(new Circle { r = 2 }));
    Map<string, int> counts = Map<string, int>();
    counts["circles"] += 1;
    print($"{shapes[0].area()} {counts["circles"]}");
    return 0;
}
```

## Building the compiler

Needs a C++20 compiler. On Windows with Visual Studio installed:

```bat
build.bat
```

This produces `bin\axon.exe` and `bin\axonc.exe` (the same program). CMake also works:
`cmake -B out && cmake --build out --config Release`.

To build Axon programs, `axon` needs a C compiler. It looks for `clang`, then `gcc`, then
MSVC's `cl` (found automatically through `vswhere`, so no Developer Prompt is needed).

## Using it

```bat
axon new mygame              &rem creates axon.toml, src\main.ax, bindings\
axon run                     &rem build + run (debug)
axon build --release         &rem optimized, debug checks removed
axon test                    &rem runs every @test function
axon run --leak-check        &rem reports memory that was never freed
axon check                   &rem errors only, no build
axon fmt [--check]           &rem formats the project
axon add vecmath --path ..\vecmath     &rem add a package (or --git <url> --tag v1.0)
axon fetch [--update]        &rem download git packages / move to their newest version
axon emit-c / emit-ir        &rem write the generated C / Axon IR to build\
axon lsp                     &rem language server (editors start it)
axon build --backend x64     &rem Axon's own x86-64 code generator instead of C
axon build --backend llvm    &rem LLVM (needs clang)
axon bindgen raylib.h -I libs\raylib\include -o bindings\raylib.ax   &rem bindings from a C header
```

Every command also takes a single file: `axon run hello.ax`, or `axonc hello.ax -o hello.exe`.

## Status against the roadmap

| Phase | State |
| --- | --- |
| **1. Tiny working language** | Done. `examples/fib.ax` prints `fib(30) = 832040`. |
| **2. Feels like Axon** | Done. `examples/adventure` is a multi-file text adventure written in Axon. |
| **3. The C bridge** | Done. `examples/raylib-square` moves a square in a raylib window; `examples/imgui-panel` adds a Dear ImGui panel (cimgui) with buttons, sliders and a checkbox. |
| **4. Safety and power** | Done. Generic structs, enums and functions (with interface bounds), `Box<T>` (also `Box<Interface>` for mixed lists), `Map<K, V>`, `ref` / `mut ref`, `Result` + `try`, debug checks removed by `--release`, `@test`, a leak checker, and a bigger standard library. Milestone: `examples/zombies`, a top-down game with `List<Zombie>`, `List<Bullet>` and `List<Box<Effect>>`; `axon test --leak-check` runs 30 seconds of play with no leaks. |
| **5. Going pro** | Done (details below). |

### Phase 5 in detail

| Item | State |
| --- | --- |
| Axon IR | Done: `axonc/src/ir.h`, `irgen.cpp`. A small low-level IR with the Windows x64 calling convention already applied. `axon emit-ir` shows it. |
| Own x86-64 backend | Done: `x64.cpp` turns IR into MASM assembly (built with `ml64`). `--backend x64` passes the whole test suite with output identical to the C backend, including C libraries (raylib, ImGui) and `@test` panics unwinding through its frames. |
| LLVM backend | Done: `llvm.cpp` turns IR into LLVM IR, compiled by `clang`. `--backend llvm` passes the whole test suite, the zombie game's leak-checked tests and `--release` builds. |
| Read C headers with libclang | Done: `cimport.cpp`. When `using c "x.h"` has no bindings file, axon reads the header with libclang and generates the bindings (saved in `build/bindings/`). `axon bindgen raylib.h -I <dir> -o bindings/raylib.ax` writes them out: 493 functions, 33 structs and 330 constants from `raylib.h`. |
| `axon fmt` | Done. Re-indents and normalizes spacing, keeps line breaks and comments, and verifies the tokens are unchanged before writing. |
| Language server | Done: `axon lsp` gives errors as you type, hover, go to definition, completion (including members after `.`), document outline and formatting. `editors/vscode` is a VS Code extension with no dependencies. |
| Packages | Done: `[dependencies]` with `path` or `git` (+ `tag` / `branch` / `rev`), `axon add`, `axon fetch`, and `axon.lock` pinning exact commits. `examples/packages` shows a library and an app. |
| Self-hosting | Done for the core language: `selfhost/axs.ax` is an Axon compiler written in Axon. `selfhost\bootstrap.bat` builds it with the C++ compiler, lets it compile itself, then compiles itself again with the result; the two outputs are byte-for-byte identical. `axs` covers structs, functions, int/bool/byte/string, `List<T>`, `ref`/`mut ref`, constants and all control flow; the full compiler (generics, interfaces, ownership checking, backends) is still the C++ one. |

The LLVM backend and header import use LLVM from `tools/llvm` (LLVM 23.1.5, unpacked from the
official release) or from `PATH`. Without LLVM, axon still works: the C and x64 backends and
hand-written bindings don't need it. `axonc\tests\test_llvm.ps1` checks everything that does.

## The language today

- Types: `int long byte float double bool char string`, `T[]`, `T?`, `List<T>`, `Map<K, V>`, `Box<T>`, `Result<T, E>`
- Generics: `struct Pair<A, B>`, `enum Option<T>`, `T max<T : Comparable>(...)`; type arguments are inferred from arguments or the expected type
- `var`, `const`, `if/else`, `while`, `do/while`, `for`, `foreach`, `switch`, `break`, `continue`, `? :`
- Structs with fields, defaults, methods, `mut` methods, `private`; interfaces after `:`; `extend` (also for built-in and C types)
- Interfaces through `ref Iface` parameters or `Box<Iface>` values (dynamic dispatch)
- Enums with data, exhaustive `switch`, `is` patterns (`x is Player p`, `item is Sword(var dmg)`, `if (!(x is T v)) return;`)
- Optionals (`none`, `??`, `== none`), errors as values (`Ok`, `Err`, `try`)
- String interpolation `$"hp: {p.health}"` and string methods (`substring contains indexOf lastIndexOf split replace trim toUpper toLower padLeft padRight repeat chars toInt toDouble ...`)
- Lists and arrays: `add pop removeAt insert clear contains indexOf sort reverse`; maps: `m[k]`, `get contains remove keys values clear`
- Ownership: moves, use-after-move errors, no moving out of fields/items, no changing a list during `foreach` or a value while a pattern borrows it, automatic cleanup, `Disposable`, `.clone()` everywhere
- Standard library: `std.io`, `std.os`, `std.text`, `std.math`, `std.random`, `std.time`
- C interop: `extern "header.h" { ... }` bindings, `using c "header.h" [as ns]`, `string` / `string?` ↔ `const char*`, `mut ref T` ↔ `T*`; `axon.toml` libraries with `include`, `lib`, `link`, `defines`

## Decisions taken on the open questions

- **Name**: Axon, `.ax` files. **`mut` on methods**: required. **Variables**: mutable by default.
- **`new`**: kept; it builds a value, never a heap object. **Error passing**: `try expr`. **`int`**: 32-bit.
- **Empty optional**: `none` (`null` is an error that points to `none`).
- **References at the call site**: `hit(mut ref p, 10)`, like C#'s `ref`.
- **Pattern bindings borrow**: `if (x is Player p)` borrows, it doesn't move.

## Known limitations

- Strings are byte strings: `length` and `substring` count UTF-8 bytes (`chars()` gives code points).
- No global variables (only `const`), no function overloading, no static methods, no closures.
- The x64 backend favors compile speed over code quality (no register allocation).

## Repository layout

```
axonc/src/        the compiler: lexer, parser, checker, C emitter, IR + x64/LLVM backends,
                  formatter, language server, driver, runtime
axonc/tests/      run tests (+ expected output), error tests, a C-interop project
editors/vscode/   the VS Code extension
examples/         fib, tour, adventure, testing, raylib-square, imgui-panel, zombies, packages
selfhost/         axs, the Axon compiler written in Axon, and bootstrap.bat
build.bat         builds bin\axon.exe with MSVC
```

Run the test suite (add `-Backend x64` to run it on the native backend):

```bat
powershell -ExecutionPolicy Bypass -File axonc\tests\run_tests.ps1
```
