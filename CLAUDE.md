# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`beans` is a programming language (`.b` files) and its toolchain, written from scratch in C++20 with no
dependencies. [SYNTAX.md](SYNTAX.md) is the language spec — it is the source of truth for grammar and
semantics questions, and it records what has been *decided* vs. what is still open. Read it before
changing language behaviour.

## Commands

```
make                      # build build/beansc (clang++, -std=c++20 -O2)
make run                  # smoke test: parse examples/hello.b + examples/tour.b
make clean                # rm -rf build/

./build/beansc lex   f.b [f2.b ...]   # token dump
./build/beansc parse f.b [f2.b ...]   # AST dump
./build/beansc check f.b [f2.b ...]   # type check, prints "ok"
./build/beansc run   f.b [f2.b ...]   # check + tree-walking interpreter
./build/beansc build f.b [-o out]     # LLVM IR + native binary (whole module if beans.mod exists)
```

`build` needs `clang` on PATH. It writes `build/<stem>.ll` and `build/beans_rt.c`, then shells out to
`clang -O2 -Wno-override-module <ll> build/beans_rt.c -o <bin>`. Read the `.ll` when debugging codegen —
LLVM's verifier catches ownership mistakes, and its complaints point at real bugs.

### The test loop

There is no test suite and no test runner. Correctness is verified by **differential testing**: the
interpreter and the native binary must produce byte-identical output, panics and exit codes included.

```
./build/beansc build examples/tour.b -o build/tour_native
diff <(./build/beansc run examples/tour.b 2>&1) <(./build/tour_native 2>&1)
```

Run this on `examples/*.b` after any change to the checker, interpreter, or codegen. `examples/tour.b`
exercises most of the language; `examples/threads.b` covers concurrency; `examples/shop/main.b` covers
multi-package programs (packages, `pub`, cross-package inheritance, block arms). `bench/*.b` have
matching `bench/*.go` for speed comparisons.

Memory is verified with Apple's `leaks` (`leaks --atExit -- ./build/<bin>`); the target is 0 leaked
bytes on every program. Small objects normally live inside pooled 64KB slabs, which hides individual
leaks from the tool — hunt real leaks with `BEANS_NO_POOL=1 leaks --atExit -- ...`, which routes every
allocation through plain calloc/free.

Only `examples/` and `bench/` are tracked — `build/` is gitignored, so scratch `.b` test programs
written there do not survive.

## Architecture

Five stages, each a file pair in `src/`, all under `namespace beans`. `src/main.cpp` is the driver and
wires them together; every stage collects errors and keeps going rather than bailing on the first one,
so one run reports many errors as `file:line:col: error: msg`.

```
loader.cpp → lexer.cpp → parser.cpp → checker.cpp →  ┬→ interp.cpp   (reference semantics)
 packages     tokens       AST           types         └→ codegen.cpp  (LLVM IR → native)
```

The loader turns the entry file into a whole `Program` (ast.h): it finds `beans.mod`, gathers one
package per directory, clones git imports into `$BEANS_HOME/src` (default `~/.beans/src`), and stamps
every top-level decl with its package-qualified name (`qualname`, e.g. `util.User`; root package and
single files keep plain names, which is why single-file behaviour is unchanged). `check`/`run`/`build`
go through it; `lex`/`parse` stay per-file.

- **lexer** — hand-written scanner, Go-style automatic newline insertion (`ends_statement`).
  `Token::text` is a `string_view` into the source; **the source string must outlive the tokens.**
- **parser** — recursive descent, precedence climbing. Owns a *copy* of the tokens because closing a
  generic can split a `>>` token into two `>` in place. `allow_struct_`/`StructGuard` disambiguates
  `Name {` as an initializer vs. the body of an `if`/`for`/`match` header.
- **checker** (`types.h`) — two passes: register all declarations and signatures, then check bodies.
  Types are interned in a `TypePool`, so `TypeId` equality *is* pointer equality. `poison` is the
  error type and stops cascading messages. Generics are checked by `unify`/`subst`.
- **interp** (`value.h`) — the **reference implementation**. When the two backends disagree, the
  interpreter is right unless there is a reason to think otherwise. Assumes the module already type
  checked and does not re-validate. Real `std::thread`, real mutexes, blocking channels. Runtime
  errors throw `BeansPanic` with line info.
- **codegen** — emits *textual* LLVM IR as a string. No LLVM library is linked.

### Adding a language feature

A feature touches five places and is not done until all of them agree:
`ast.h` (node) → `parser.cpp` → `ast_print.cpp` (dump) → `checker.cpp` → `interp.cpp` → `codegen.cpp`,
then the run-vs-native diff. If codegen cannot handle it yet, report it through `CG2::err`, which
appends "— not in the native backend yet (beansc run still works)" so `beansc run` stays the escape
hatch.

Note the type system exists **twice**, deliberately: `beans::Type` in `types.h` for checking, and the
smaller `Ty` inside `codegen.cpp` for lowering. They are separate structures; a new type must be added
to both. Likewise `Decimal` in `value.h` (used by the interpreter *and* for compile-time constant
folding in codegen) is mirrored by `BDec` in the C runtime — the two must compute identically.

### Inside codegen.cpp

The single largest file. Structure, top to bottom:

- `Ty` / `CImpl` — codegen's type model and one monomorphized class instantiation.
- `CG2` — module-level state: type interning, class instantiations, the global **selector table**
  (every method name gets an index; every `CImpl` gets a vtable spanning the full selector set), the
  string pool, and two worklists (`impl_queue` for class instantiations, `fn_queue` for generic
  function instances). Both queues can grow *while* emitting, so the driver drains them together.
- `FreeVars` / `ClosureScan` — free-variable analysis. Closures are lambda-lifted; captured variables
  are moved into shared heap cells (`boxed_names`) so mutation and escape both work. Lifted bodies
  land in `cg.lifted`, appended after the main function text.
- `FnEmit` — per-function emitter, and where the **reference counting** discipline lives.
- `CodeGen::runtime_c()` — the entire C runtime returned as one raw string literal: RC header,
  destructors, strings, lists, maps, decimal, pthreads, channels.

### Reference counting rules (the part that is easy to break)

Native binaries use ARC, no GC. Every heap value carries a 16-byte header (count + `meta` describing
its pointer layout, so one generic destructor can walk nested values). String constants are immortal
(`BEANS_IMMORTAL`). Count ops are plain until the first `thread.spawn` flips `cc_mt`, then atomic.
The count word also carries the allocator size class in bits 48-59 (`RC_CLS_SHIFT`), so **any test of
the count must mask with `RC_COUNT`** — a raw `h->rc == 0` is never true for a pooled object. Small
objects come from per-thread size-class freelists over registered slabs; `BEANS_NO_POOL=1` disables
the pool.

RC is deliberately kept off hot paths — **function arguments, loop variables, and reads borrow; they
do not retain.** Only frame-owned locals (`Var::owned`) and mid-statement temporaries (`temps`) hold
refs. When editing `FnEmit`:

- Temporaries created while emitting a statement go on `temps` via `own()`, are removed by `consume()`
  when ownership transfers, and are released by `flush_temps(mark)` at the statement end.
- **Release branch-local temporaries inside their own branch.** LLVM's verifier rejects the IR
  otherwise, and it is right — the value is not live on the other path.
- **A captured parameter needs a retain going into its heap cell**, or it is over-released.
- **Runtime calls that store take ownership** — `beans_map_set` owns key and value; every call site
  must `transfer_in` both first. A borrowed pointer stored in a container was the one heap-corruption
  bug this codebase has had.
- Release cascades are **iterative**: `beans_release` drains an explicit stack (and the interpreter's
  `~Value` parks children in `TeardownPile`), so a dropped 400k-node chain cannot smash the C stack.
  `examples/deep.b` is the regression test; keep both paths non-recursive.
- Known gap, documented on purpose: a `?` early return can hold mid-statement temporaries slightly
  longer than needed.

Reference cycles are collected by trial deletion (Bacon–Rajan) in the C runtime, all `cc_`-prefixed:
meta bits 61-62 are the color, bit 63 the in-buffer flag, so **shape reads must mask with `CC_SHAPE`**
(a raw `meta >> 3` sign-extends garbage once a color is set). A dec-to-nonzero on a pointer-bearing
shape parks the object in `cc_roots`; `cc_collect()` runs from `beans_alloc` (never inside a release
cascade — a mid-destroy object would be freed under the walker) and only when `cc_threads == 0`
(worker threads are mutators the collector cannot pause), plus an `atexit` sweep. All phases are
iterative over an explicit stack, whites are batch-freed after all walking so no stale pointer is
ever read, and `destroy` was split into `cc_release_children` + `cc_free_shell` so a parked object
released to rc 0 becomes a black husk whose shell the collector frees later. `examples/cycles.b`
is the diff-test for all of this; pointer-slot masks stop at meta bit 60 (≈55 fields), enforced in
`request_impl`.

## Conventions in this codebase

- Comments explain *why* and record hard-won bugs; they are sparse and load-bearing. Match that density.
- Snake_case for functions and variables, matching the language's own style rule.
- `// ---- section ----` banners divide the large `.cpp` files.
- Imports are fully resolved. `std.io`/`std.thread` stay wired to builtins; everything else is a real
  package. The **checker does all cross-package resolution once** and writes it into the AST
  (`Expr::resolved`, `TypeRef::resolved`, `ClassDecl::supers_resolved`); the interpreter and codegen
  consume those annotations instead of re-resolving imports. Their plain-name fallback (`qual()`, the
  current-package prefix) exists for one reason: string-interpolation segments are re-parsed inside
  interp/codegen and never saw the checker — when touching resolution, keep both paths working.
  `examples/shop/` is the multi-package diff-test target next to `examples/tour.b`.
