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
| LLVM backend | next |

## Build

```
make        # builds build/beansc
make run    # parses the example files
```

- `beansc lex file.b` — dump the token stream
- `beansc parse file.b` — parse and dump the AST
- `beansc check file.b` — full type check (prints `ok` or errors)
- `beansc run file.b` — check, then execute

The interpreter is the reference implementation: exact `decimal` math, real OS threads for `thread.spawn`, real mutexes and blocking channels, `defer`, dynamic dispatch, and runtime panics with line numbers. The LLVM backend will be tested against its behavior.

Errors print as `file:line:col: error: message`; the parser recovers and keeps going so you see many errors at once.
