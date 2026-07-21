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
| LLVM native backend | v3 — whole language native (closures, threads, maps, generics) |

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

Known v1 memory model: native heap values are never freed. Reference counting is the next big piece.

## Benchmarks (Apple Silicon, clang 21 -O2 vs go 1.26.4)

| benchmark | beans (native) | Go | interpreter |
|---|---|---|---|
| fib(40) | **0.31s** | 0.34s | — |
| 200M-iteration `sum += i % 7` | **0.08s** | 0.18s | — |
| 100M virtual method calls (interfaces) | **0.12s** | 0.36s | — |
| fib(30) | ~0.003s | — | 28s |

Same outputs everywhere. The design bet — beans semantics + LLVM's optimizer beats Go's compiler — holds on every benchmark so far, and the OOP one is the biggest win: LLVM sees through beans' constant vtables, Go's interface dispatch stays opaque.

Errors print as `file:line:col: error: message`; the parser recovers and keeps going so you see many errors at once.
