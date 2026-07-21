# beans

A small OOP language: Java-style objects, Go-sized grammar, C++-level access, built to be faster, handy and useful. Files end in `.b`.

- [SYNTAX.md](SYNTAX.md) — the language draft (start here)
- [examples/](examples/) — real `.b` programs
- [src/](src/) — the toolchain, C++20, no dependencies

## Status

| piece | state |
|---|---|
| syntax draft | v0.3 |
| lexer | done |
| parser | done |
| type checker | done (v1) |
| interpreter | done (v1) — `beansc run` |
| LLVM native backend | v4 — whole language native + reference-counted memory |

## Build

```
make        # builds build/beansc
make run    # parses the example files
```

- `beansc lex file.b` — dump the token stream
- `beansc parse file.b` — parse and dump the AST
- `beansc check file.b` — full type check (prints `ok` or errors)
- `beansc run file.b` — check, then execute (reference interpreter)
- `beansc build file.b [-o out]` — compile to a native binary via LLVM

The interpreter is the reference implementation: exact `decimal` math, real OS threads for `thread.spawn`, real mutexes and blocking channels, `defer`, dynamic dispatch, and runtime panics with line numbers.

The native backend emits textual LLVM IR and hands it to clang — no LLVM library dependency. v3 covers the whole language: classes (vtable dispatch, inheritance, interface defaults, `override`, `as?`), monomorphized generics on classes *and* functions, enums + `match`, Option/Result + `?`, exact `decimal`, lists and maps, closures (lambda-lifted, captured variables live in shared heap cells — mutation works, escaping works), real pthreads for `thread.spawn`/`Mutex`/`Channel`/`AtomicInt`, `defer`, and string interpolation. Every test file produces byte-identical output under `beansc build` and `beansc run` — panics included, same message, same exit code.

## Memory

Native binaries use automatic reference counting — no GC, no pauses. Every heap value carries a 16-byte header (atomic count + shape info); the compiler emits retains and releases at ownership boundaries and a generic destructor walks nested structures. String constants are immortal. Verified with Apple's `leaks` tool: **0 leaked bytes** on every test program, and a 1M-iteration allocation stress test (object + interpolated strings + decimal math per pass) runs in **1.4MB flat**.

The design keeps RC off hot paths: function arguments, loop variables, and reads borrow instead of retaining, so the benchmark numbers above are measured *with* RC enabled. Known limits: reference cycles leak (cycle detection later, Nim-style), and a `?` early-return can hold mid-statement temporaries a little longer.

## Benchmarks (Apple Silicon, clang 21 -O2 vs go 1.26.4)

| benchmark | beans (native) | Go | interpreter |
|---|---|---|---|
| fib(40) | **0.31s** | 0.34s | — |
| 200M-iteration `sum += i % 7` | **0.08s** | 0.18s | — |
| 100M virtual method calls (interfaces) | **0.12s** | 0.36s | — |
| fib(30) | ~0.003s | — | 28s |

Same outputs everywhere. The design bet — beans semantics + LLVM's optimizer beats Go's compiler — holds on every benchmark so far, and the OOP one is the biggest win: LLVM sees through beans' constant vtables, Go's interface dispatch stays opaque.

Errors print as `file:line:col: error: message`; the parser recovers and keeps going so you see many errors at once.
