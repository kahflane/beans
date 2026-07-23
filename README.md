# beans

A small OOP language: Java-style objects, Go-sized grammar, with C++-class performance and systems access as goals. Files end in `.b`.

- [SYNTAX.md](SYNTAX.md) — the language draft (start here)
- [examples/](examples/) — real `.b` programs
- [src/](src/) — the toolchain, C++20, no dependencies

## Status

| piece | state |
|---|---|
| syntax draft | v0.7 |
| lexer | done |
| parser | done |
| module loader | done (v6) — modules, git imports, compiler-shipped `.b` std packages |
| type checker | v3 — whole-program HIR types, target layouts, MIR ownership plan |
| interpreter | done (v2) — `beansc run` |
| LLVM native backend | v6 — whole language native, RC + cycle collector |

## Build

```
make        # builds build/beansc
make run    # parses the example files
make test   # interpreter/native differential suite
make test-sanitize
make bench-quick
make bench-verify
make bench-full
make bench-profile NAME=trees
```

- `beansc lex file.b` — dump the token stream
- `beansc parse file.b` — parse and dump the AST
- `beansc check file.b` — full type check (prints `ok` or errors)
- `beansc run file.b` — check, then execute (reference interpreter)
- `beansc build file.b [-o out]` — compile to a native binary via LLVM
- `beansc build --release --lto --cpu native file.b [-o out]` — optimized native build

`check`/`run`/`build` load the whole program: if a `beans.mod` sits next to the file, every `.b` file in that directory joins the root package, `import shop.util` pulls `util/`, and `import github.com/owner/repo` clones the repo into `~/.beans/src` on first use (`require <path> <tag>` in beans.mod pins a git tag). No beans.mod = plain single file, as before. [examples/shop/](examples/shop/) is a working three-package program.

The interpreter is the reference implementation: exact `decimal` math, real OS threads for `thread.spawn`, real mutexes and blocking channels, `defer`, dynamic dispatch, and runtime panics with line numbers.

The low-level layer has started: `RawPtr<T>` can allocate and access primitive
scalar, raw-pointer, fixed-array, or nested `extern "C" struct`/`union` memory
inside an explicit `unsafe {}` block, including LLVM volatile
loads/stores and sequentially consistent raw integer atomics. Top-level
`extern "C" fn` declarations can call mixed integer, bool, raw-pointer,
floating-point, and C-layout aggregate functions in both backends. `Simd4f32` is a real inline
four-lane LLVM vector with arithmetic, reduction, and unaligned-safe raw
load/store. `[T; N]` is an inline fixed array for inline scalar, pointer, array,
and struct elements, and `Slice<T>` is an inline pointer/length view with
checked access over raw-compatible memory. `struct` values copy, pass, and return inline; `extern "C" struct`
uses target C field order and alignment and can be read or written through
`RawPtr` and `Slice`. `extern "C" union` adds overlapping scalar storage with
field access kept behind `unsafe`. Struct and union fields accept nested inline
values; infinite inline recursion is rejected. `extern "C"` accepts these
records by value in arguments and returns. Small generated C wrappers leave
the target-specific calling convention to Clang in native and interpreter
paths. Extern parameters can also take borrowed synchronous C callbacks made
from Beans closures or stored top-level functions. Callback values may cross
the ABI only for the duration of that call and on the same thread.

The native backend emits textual LLVM IR and hands it to clang — no LLVM library dependency. The C runtime lives in `runtime/beans_rt.c`, not inside the compiler binary. Development builds link a cached runtime object; `--release --lto` links cached runtime bitcode so LLVM can optimize across the boundary. `BEANS_RUNTIME` can point at another runtime source. The backend covers the whole language: classes (descriptor/vtable dispatch, inheritance, interface defaults, `override`, `as?`), monomorphized generics on classes *and* functions, enums + `match` (block-bodied arms included), Option/Result + `?`, exact-width integers and `f32`, exact `decimal`, lists and maps, closures (lambda-lifted, captured variables live in shared heap cells — mutation works, escaping works), real pthreads for `thread.spawn`/`Mutex`/`Channel`/`AtomicInt`, `defer`, string interpolation, and multi-package programs (symbols are package-qualified; cross-package calls, inheritance, generics, and interface dispatch all compile into one flat module). Every test file produces byte-identical output under `beansc build` and `beansc run` — panics included, same message, same exit code.

High-level standard-library code can now be written in Beans. The loader ships
packages from `lib/std/`; `std.collections`, `std.math`, `std.bytes`,
`std.path`, `std.fmt`, `std.fs`, and `std.reader` are the first ones. Generic collection
`filter`/`transform`, inout Map increment/insert/merge/remove/map policies,
Option and Result combinators, `frequencies`, `unique`, `gcd`, `clamp_int`,
CRC32, unsigned varint append/encoding/decoding, path handling,
integer hex/binary/group formatting, high-level whole-file text/byte/write/copy
helpers, and buffered line reading are normal `.b` functions. Only their current
low-level storage operations remain native. The scored bytes workload calls the
Beans-written varint and CRC32 code, not the older native compatibility methods.
Set `BEANS_STDLIB` to use a
different shipped library root.

## Memory

Native binaries use automatic reference counting **plus a cycle collector** — no tracing GC, no pauses on the straight-line path. Every heap value carries a 16-byte header (atomic count + shape info); the compiler emits retains and releases at ownership boundaries and a generic destructor walks nested structures. String constants are immortal.

Reference cycles (`a.next = some(b); b.next = some(a)`) are caught by trial deletion (Bacon–Rajan, the Nim ORC family): a decrement that doesn't hit zero parks the object as a possible cycle root; when enough roots pile up, the collector trial-deletes each root's subgraph, restores anything still externally referenced, and frees the rest. It runs only between statements when no worker threads are live, and once more at exit. All walks are iterative — a 300k-node dropped ring is fine.

Verified with Apple's `leaks` tool: **0 leaked bytes** on every test program — including [examples/cycles.b](examples/cycles.b), which drops 400k cycle pairs, a self-cycle, a 300k ring, and a closure that captures its own cell. **2M dropped cycle pairs run in 1.4MB flat**, same as the acyclic stress test, and live rings survive collections untouched.

The design keeps RC off hot paths: function arguments, loop variables, and reads borrow instead of retaining. `move local` moves an owned value with compile-time use-after-move checks, and `return move local` transfers its last reference instead of retaining it. List, Map, OrderedMap, `Box<T>`, and the typed append-only `Arena<T>` are move-only outer handles; collections copy only through explicit `clone()`, and Arena values drop in bulk on `clear` or scope exit. `Shared<T>`/`Weak<T>` add an explicit atomic control block for cross-thread ownership without making local classes pay that cost. `Send`/`Sync` interface bounds are enforced, and `thread.spawn` rejects non-`Send` captures and returns. Pointer-valued `Option` uses a null niche in native code, while structs, fixed arrays, SIMD vectors, slices, and nested wide Options use an inline `{has_value, payload}` aggregate. A Result with a wide branch is also inline. Ordinary structs can own ARC fields. Typed-width List, Map-value, Box, Arena, Shared, Mutex, Channel, Thread-result, and user-enum payload storage keeps wide values and 16-byte decimals inline with ARC pointer masks. Map keeps its existing narrow fast path and uses a parallel buffer only for wide values. Wide value keys are boxed once when stored; lookup uses a stack copy and generated field-wise equality and hashing, so queries do not allocate. The compiler tracks nested references through copies, calls, captures, assignments, class nesting, collection operations, matches, and `?`. Inline Option/Result forms do not allocate their own aggregate box; user enums remain ARC values but keep wide payloads inline inside that allocation. The benchmark numbers below are measured *with* ARC and the collector enabled. Known limits: collection is deferred while worker threads run (a program that churns cycles forever while never letting its threads drain will grow until they do), nested move-only collection clones, consuming Map reads, and a `?` early-return that can hold mid-statement temporaries a little longer.

## Benchmarks

The benchmark harness compares safe Beans with both tuned C++ and C++ using
Beans-like ownership. It uses runtime inputs, fixed checksums, randomized run
order, cold-start separation, process CPU/RSS data, and raw JSON samples. Full
mode uses ten timing batches and at least ten measured seconds per target.
Rows above 3% variation are retried with longer batches; discarded attempts
remain in the JSON.

The stable 39-workload full run on an Apple M1 (2026-07-23) scored **103.4% of
tuned C++** and **128.9% of matched C++**. Peak memory was **1.17x tuned C++**
and **0.97x matched C++**. Every group cleared the 70% floor, so both the safe
80% target and tuned 90% target passed. This is a result for the recorded suite,
machine, compiler, flags, and working tree—not a general claim that every Beans
program is within 10% of C++.

The weakest tuned-C++ rows are still clear: exact Decimal arithmetic 48.6%,
the mixed allocation application 55.0%, Option-heavy chains 68.4%, UTF-8
walking 70.3%, and slices 72.2%. Those are the next compiler/runtime targets.
Beans wins the sequence, OrderedMap, deep teardown, churn, and tree rows. The
generated `bench/report.md` contains every median, CV, group score, memory
result, compile time, and binary size.

`make bench-verify` builds every target and enforces fixed checksum and
byte-output parity. `make bench-quick` is useful during development but is
never claim-eligible. Timed runs write `bench/report.md` and
`build/bench/results-<mode>.json`.

`kv_store` measures the append/restart/compact algorithm in memory so storage
hardware does not enter the compiler score. File and mmap tests belong in the
systems report. C++ has no cycle collector, so the cycle baselines explicitly
break their test cycles; the report keeps that difference visible.

Errors print as `file:line:col: error: message`; the parser recovers and keeps going so you see many errors at once.
