# beans

A small OOP language: Java-style objects, Go-sized grammar, C++-level access, built to be faster than Go. Files end in `.b`.

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
| LLVM native backend | v1 — `beansc build` (numeric/string/fn core) |

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

The native backend emits textual LLVM IR and hands it to clang — no LLVM library dependency. v1 covers ints, floats, bools, strings (+interpolation), functions, and all control flow. Classes, enums, generics, decimal, and threads still run under `beansc run` and move to native next.

## Benchmarks (Apple Silicon, clang 21 -O2 vs go 1.26.4)

| benchmark | beans (native) | Go | interpreter |
|---|---|---|---|
| fib(40) | **0.31s** | 0.34s | — |
| 200M-iteration `sum += i % 7` | **0.08s** | 0.18s | — |
| fib(30) | ~0.003s | — | 28s |

Same outputs everywhere. The design bet — beans semantics + LLVM's optimizer beats Go's compiler — holds on both benchmarks.

Errors print as `file:line:col: error: message`; the parser recovers and keeps going so you see many errors at once.
