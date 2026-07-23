# beans syntax — draft 0.7

Language: **beans** · extension: **.b** · status: draft for discussion
Toolchain status: **lexer + parser + loader + checker + interpreter + native backend implemented** — including multi-file modules and git imports

## What beans is for

Two target jobs, and the design serves both:

1. **Business apps** (accounting, ERP, billing) → mandatory explicit types, `decimal` money math, no null, no exceptions, boring and readable.
2. **Systems work** (databases, OS/hardware control) → sized ints, value types, `unsafe` layer with raw memory and C/C++ interop, no GC pauses.

## Design rules

1. Small grammar. Every keyword must remove more complexity than it adds.
2. Everything is an object. `5.abs()` works. Primitives are unboxed under the hood.
3. No null. No exceptions. `Option<T>` and `Result<T>` only.
4. **Every new name states its type.** No inference. Applies to `let`/`var`, params, fields, loop variables. One exception: match bindings — the matched value already pins the type, so `some(u) =>` is fine (`some(u: User) =>` allowed if you want it).
5. snake_case for functions, methods, variables, packages, enum variants. PascalCase for types. lowercase for primitives.
6. Private by default — classes, interfaces, enums, functions, methods, fields. `pub` is the only way to expose anything outside its package.
7. If two designs work, pick the one with less syntax.

### Why `Option` is uppercase but `some` is lowercase

The case tells you what a thing is: PascalCase = type, snake_case = value. `Option<User>` is a type. `some(u)` is a value. And they're not special — Option and Result are just built-in enums, and beans enum variants are snake_case:

```
enum Option<T> {
    some(value: T)
    none
}

enum Result<T, E> {
    ok(value: T)
    err(error: E)
}
```

Rust capitalizes `Some`/`Ok` because Rust variants are PascalCase. Beans variants are snake_case, so lowercase is the consistent choice, not an accident.

## Files, modules, imports (implemented, v0.4)

- One folder = one package. `.b` files in it share the package — no import needed between them.
- Everything is private to its package unless marked `pub`.
- Entry point: `fn main()`, in the module root.

A module is a directory tree with a `beans.mod` at its root:

```
module shop
require github.com/acme/http v1.2    // optional git tag pins
```

Imports are Go-style — std by dot path, local packages by module path, libraries straight from a git host:

```
import std.io
import std.thread
import shop.util                     // <root>/util/*.b, used as util.thing
import shop.money.fx                 // nested: <root>/money/fx/
import github.com/acme/http          // cloned to ~/.beans/src on first build
import gitlab.com/tools/csv as csvlib
```

- Last path segment is the name you use (`http.get(...)`). `as` renames the binding.
- Cross-package access: `util.some_fn()`, `util.User`, `new util.User(...)`, `util.color.red` — anything `pub`. Methods of a `pub interface` travel with it (an interface is its method set).
- `pub fn init(...)` controls class construction across package lines. Struct field literals still enforce field visibility.
- A git import needs `host/owner/repo`; the repo must carry its own `beans.mod`. First build clones (`--depth 1`, `--branch` if pinned) into `$BEANS_HOME/src` (default `~/.beans/src`) and reuses the cache after.
- No `beans.mod` above the file = single-file mode: `std.*` and git imports still work, local packages don't.
- Two packages can't share a final name in one program (`a/json` + `b/json`) — rename one directory. `beans.lock` with exact hashes: later.

## Lexical

- No semicolons. Newline ends a statement (Go-style: only after a token that can end one).
- Style consequence, same as Go: `} else {` must be on one line.
- Comments: `//` line, `/* */` block (nesting allowed).
- Number literals can use `_` separators: `1_000_000`. Hex `0xFF`, binary `0b1010`.
- No parens around conditions: `if x > 3 { }`. Braces always required.

## Strings

- `"..."`, immutable, UTF-8.
- Interpolation with `{}`: `"hi {name}, total {price * (qty as decimal)}"`.
- Format specs ride after a `:` in the braces: `{x:8}` pads to width 8 (right-aligned),
  `{x:-8}` left-aligns, `{pi:.2}` fixes decimals (float/decimal only), `{pi:8.2}` both.
  Width pads anything printable — `{xs:12}` pads a whole list. Same rendering as `std.fmt`.
- **There is no `+` for strings.** Building strings happens through interpolation, `std.fmt` (sprintf-style: padding, precision, alignment), or `list.join(sep)`. One way to do it, and it's the readable one.
- Escapes: `\n \t \r \0 \\ \" \{ \}`.

**Methods (v0.5, implemented, byte-based — unicode arrives later as explicit `chars()`, `len` stays bytes forever):**
`len`, `is_empty`, `first(n)`, `last(n)`, `slice(from, to)` (half-open, panics out of range),
`byte_at(i)` (panics), `contains`, `starts_with`, `ends_with`, `find`/`rfind -> Option<int>`
(empty needle: `find` says 0, `rfind` says len), `trim`/`trim_start`/`trim_end` (ASCII whitespace),
`to_upper`/`to_lower` (ASCII), `replace(old, new)` (all occurrences; empty `old` changes nothing),
`repeat(n)` (panics on negative), `split(sep) -> List<string>` (keeps empties; empty sep = one piece),
`lines() -> List<string>` (a trailing newline makes no empty final line),
`to_int`/`to_float`/`to_decimal -> Result<...>`,
`chars() -> List<string>` (UTF-8 characters; malformed bytes come through one at a time),
`count_chars(from, to)` for a checked, allocation-free byte range scan,
`find_byte(byte, from) -> int` (`-1` when absent), `range_equals(from, to, other)`,
and `parse_int_range_or(from, to, fallback)` for allocation-free byte-range work.

## Bytes (v0.5, implemented)

The binary buffer — strings stay text; anything binary is `Bytes`. Mutating methods return
self, so page-building chains work: `new Bytes(4096).put_u32(0, root).put_u64(8, lsn)`.

- `new Bytes(n)` (zeroed, panics on negative), `Bytes.from(s)` (copies the text bytes)
- `len()`, `reserve(n)`, `resize(n)` (regrown range reads zero), `fill(v)`
- `get(i)` / `set(i, v)` — one byte, panics out of range; `push(v)` appends one
  byte. These are low-level storage operations used by Beans-written formats.
- `get_u8/u16/u32/u64/i64(pos)` / `put_...(pos, v)` — fixed width, little-endian, panics out of range
- `slice(from, to)`, `copy_from(src, at)`, `append(other)`, `append_str(s)`,
  `append_i64(v)` (little-endian), `append_range(src, from, to)` (no slice allocation)
- `to_string()` — as text, stops at an embedded NUL; `to_string_full()` copies every
  byte, including NUL (used by binary-safe source packages such as `std.reader`)
- `==` / `!=` compare by value: length, then contents
- `append_varint(v)` / `get_varint(pos)` — unsigned LEB128 over the 64-bit pattern
  (negatives take 10 bytes); `Bytes.varint_size(v)` says how far to advance
- `crc32(from, to)` — IEEE crc32 of a range, panics out of range

## Files and the OS (v0.5, implemented)

Class-first, like everything builtin. Errors are `Result<T>`; `Error.kind` carries a slug
(`not_found`, `permission`, `exists`, `is_dir`, `not_dir`, `not_empty`, `closed`, `io`) and
`Error.msg` is `path: OS message`.

- **File statics/intrinsics**: `exists`, `size`, `remove`, `rename`, and
  `open(path, mode)` → `Result<File>` with modes `"r"`, `"rw"`,
  `"create"`, `"append"`.
- **std.fs**: Beans-written `read`, `read_bytes`, `write`/`append`, `write_bytes`/
  `append_bytes`, and `copy`. These compose `File.open`, positional/cursor I/O,
  truncate, close, and exact byte-to-string conversion; only that low-level layer
  stays native. The old native `File.read(path)` helper is gone.
- **File methods**: positional I/O first — `read_at(pos, n)` → `Result<Bytes>` (short read at
  EOF returns what's there), `write_at(pos, b)`; cursor `read(n)`/`write(b)`; `seek`/`seek_end`
  (return the new position, panic on a closed file), `tell`, `size`, `truncate`, `sync` (fsync —
  the durability call), `close` (double close is an error result). Dropping the last reference
  closes the fd as a safety net; `close()` is still the API.
- **File locks**: `lock()` (blocking, exclusive), `try_lock()` (`ok(false)` means someone else
  holds it), `unlock()` — advisory flock, owned by the open file description, so two handles on
  one file contend. Single-writer databases.
- **Dir statics**: `make`, `make_all`, `list` → `Result<List<string>>` (sorted), `remove`
  (empty only), `remove_all` (recursive), `exists`, `temp`, `sync` — fsync a directory, the
  rename-commit pattern's second half; `walk(path)` → `Result<List<string>>` — recursive,
  files and symlinks only (never follows a link), paths relative to the argument, sorted.
- **std.path** (pure Beans string math, no fs access): `join(a, b)` (absolute `b` wins),
  `parent`, `base`, `ext` (with the dot; a leading dot is a dotfile, not an extension),
  and `stem`. Import it with `import std.path`; the old native `Path.*` copy is gone.
- **std.reader**: `new reader.Reader(f)` then `read_line()` → `Result<Option<string>>` —
  `ok(some(line))` without its newline, a partial last line, then `ok(none)` at EOF.
  It reads at its own offset (pread), so the file's cursor never moves; buffered
  data keeps serving after `f.close()`, and the closed error surfaces on the next refill.
  Buffering and line policy are Beans source; only `File.read_at` stays native. The old
  native `BufReader` type is gone.
- **std.os**: `args()` (`beansc run f.b -- a b` passes them; the native binary uses argv),
  `env(name)` → `Option<string>`, `exit(code)`, `now_ms`, `ticks_ms`, `sleep_ms`.
- **std.io**: `println`/`print`, `eprintln`/`eprint` (stderr), `read_line()` → `Option<string>`

High-level compiler-shipped packages are normal Beans source under `lib/std`.
`std.collections` provides `count_int`, `sum_int`, `frequencies`, `unique`, and
the generic `count`, `filter`, `transform`, and `unique_of` functions. Its
`increment`, `get_or_insert`, `merge_with`, `remove_if`, and `map_values`
functions mutate a caller Map through `inout`; these are ordinary generic Beans
functions, including for structural wide keys.
`Option` provides instance methods `map`, `and_then`, and `filter`; `Result`
provides instance methods `map`, `and_then`, and `recover`. There are no
`std.option` or `std.result` packages. `std.math`
provides `clamp_int` and `gcd`; `std.bytes` provides Beans-written `crc32`,
`varint_size`, `encode_varint`, `append_varint`, `decode_varint`, and
`decode_varint_at_or`; `std.path` is fully
Beans-written; `std.fmt` implements `hex`, `bin`, and `group` in Beans; and
`std.fs` implements the high-level whole-file byte/write/copy helpers in Beans;
`std.reader` implements buffered line reading in Beans.
Floating-point/decimal conversion and guarded padding remain native. These go through
the same checker, interpreter, MIR, and native backend as user code.
`BEANS_STDLIB` can point the loader at another shipped-library root. Native
registry rows remain for low-level allocation/storage, raw bytes, OS calls,
atomics, and thread entry while more of `core` and `std` move to `.b` files.

**What prints** (same rule for `io.println` and `{x}` interpolation): numbers, bools, strings;
enums, as `variant` or `variant(payload, ...)`; lists of printable things, as `[a, b, c]`,
nesting included — and `join(sep)` renders the same way. Maps and class instances don't print
yet — give them a string form first. (`Result` carries an `Error` object, so it stays
unprintable too — match on it.)

[examples/kv.b](examples/kv.b) is the proof: an append-only KV store with binary records and a
durable compaction (write temp, sync, rename over, sync the parent dir).

## MMap (v0.5, implemented)

A shared mapping of a whole file — the page-cache path a database wants. One writer, no
collector interaction: the mapped region is not beans heap, only the handle is.

- `MMap.open(path, writable)` → `Result<MMap>` — maps the entire file (`MAP_SHARED`); the
  handle keeps its fd for `resize`, and the mapping outlives the path — unlink while mapped
  is fine. An empty file maps with `len() == 0`.
- `len()`; `get_u8/u16/u32/u64/i64(pos)` and `put_...(pos, v)` — little-endian, bounds-checked
  panics; `put` panics on a read-only map; `put`/`write` return self for chains.
- `read(pos, n)` → `Bytes` (copy out), `write(pos, b)` — panics out of range.
- `flush()` / `flush_range(pos, n)` → `Result<bool>` (msync — the durability call),
  `close()` → `Result<bool>` (double close is an error; access after close panics).
- `resize(n)` → `Result<bool>` — ftruncate + remap in place, grow or shrink; read-only
  maps refuse with a `permission` error. On a remap failure the handle stays open but empty.
- Dropping the last reference unmaps (and closes the fd) as a safety net.

## std.fmt (v0.5, implemented)

Interpolation assembles, fmt formats. No printf — the language has no varargs.

- `pad_left(s, width)` / `pad_right(s, width)` — spaces, byte width; already-wide input
  comes back unchanged.
- `float(x, places)` — fixed decimals (`3.14`), places clamped to 0..100.
- `dec(d, places)` — exact decimals: rounds half away from zero when narrowing, zero-pads
  when widening. `fmt.dec(19.995, 2)` is `"20.00"`.
- `hex(n)` / `bin(n)` — the 64-bit two's-complement pattern, lowercase, no prefix:
  `hex(-1)` is 16 f's.
- `group(n, sep)` — thousands grouping: `group(1234567, ",")` is `"1,234,567"`.

## Variables

```
let x: int = 5              // can't be reassigned (like Java final)
var total: decimal = 0.0    // can be reassigned
```

`let` means the *variable* can't be rebound. The object it points to can still change inside (Java-style — no borrow checker, no `mut` markers).

`move name` moves the value out of a local binding. The old binding cannot be
read again unless it is a `var` and gets a new value first:

```
var job: Job = next_job()
let running: Job = move job
job = next_job()                 // reinitializes it
```

The checker rejects use after move and a value moved on only one branch. A
move on every branch is definite. Normal parameters, loop variables, match
bindings, and closure captures are borrowed, so they cannot be moved. Moving an outer
local from a loop is also rejected because the next iteration would see an
empty binding. For now `move` names a whole local; field and index moves need
consuming accessors such as List `remove`.

Parameters borrow by default. A `move` parameter owns its argument and drops it
at function exit unless the body moves it onward:

```
fn enqueue(move jobs: List<Job>) { ... }

var batch: List<Job> = make_batch()
enqueue(move batch)
```

A fresh result can be passed directly; an existing move-only local needs
`move`. Move modes must match across interface methods and overrides. Function
values and closures do not carry ownership modes yet, so a function with move
or inout parameters cannot be stored as a closure value.

An `inout` parameter aliases one mutable caller local for the duration of the
call. It is not copy-in/copy-out:

```
fn swap(inout left: int, inout right: int) {
    let old: int = left
    left = right
    right = old
}

var a: int = 1
var b: int = 2
swap(inout a, inout b)
```

The caller must write `inout`, the argument must be a `var`, and the same local
cannot appear in two inout positions of one call. An inout parameter cannot be
captured by a closure. Native code passes the local's address directly; ARC
values release the old value and own the replacement on overwrite.

`unique class` gives the same outer-handle rule to a user type:

```
unique class Packet {
    bytes: Bytes
}
```

It cannot be copied by binding, assignment, return, or storage. Use `move` to
move it. A subclass of a move-only class is move-only too. This controls the
reference handle; fields inside the object still follow their own rules.

`Box<T>` and `Arena<T>` are move-only outer handles. Binding or assigning an
existing handle needs `move`; function parameters borrow by default.

```
var value: Box<int> = new Box(7)
value.set(9)
let owned: Box<int> = move value

var arena: Arena<string> = new Arena(1024)
let handle: int = arena.put("bean")
let word: string = arena.at(handle)       // checked; panics on a bad handle
let maybe: Option<string> = arena.get(handle)
arena.clear()                             // drops all values in one pass
```

`new Box(value)` owns one heap slot. `get()` returns the value and `set(value)`
replaces it. The native runtime uses the common iterative ownership walker, so
boxed chains do not recurse during teardown. Structs, fixed arrays, SIMD,
slices, inline Option/Result values, and decimals keep their real inline layout;
nested ARC fields are retained and dropped recursively.

`new Arena(capacity)` needs a declared `Arena<T>` type or an explicit type argument. `put(value)` appends and
returns a stable integer handle; `len`, `at`, `get`, and `clear` operate on the
current region. `clear` keeps capacity but invalidates every old handle. This
arena stores typed-width values in one contiguous region. Wide values and
16-byte decimals stay inline, and `clear` drops every nested ARC field before
reusing the buffer. References returned by `get` and `at` are owned copies;
there is no borrowed arena-reference type yet.

`Shared<T>` is the explicit thread-safe shared-ownership handle. `Weak<T>`
observes the same control block without keeping the value alive:

```
let shared: Shared<string> = new Shared("beans")
let weak: Weak<string> = shared.downgrade()
let live: Option<Shared<string>> = weak.upgrade()
let gone: bool = weak.expired()
```

`get()` returns a copy of the value. Wide values and decimals stay inline in a
typed payload box, and nested ARC fields use normal copy ownership. The control block owns one value reference
until its last strong handle dies; upgrade uses an atomic compare/exchange, so
it cannot revive a dead value. A cycle made through `Shared` must be broken with
`Weak`, like C++ `shared_ptr`/`weak_ptr`; the local-class cycle collector does
not trace through explicit Shared control blocks. `Shared<T>` and `Weak<T>` are
`Send` and `Sync` only when `T` is both. `Mutex<T>` is the explicit lock-based
synchronization boundary, including for local ARC class values.

### Struct and collection literals

Structs keep named field literals. Lists and maps keep their literal forms:

```
let point: Point = Point { x: 3, y: 4 }
let values: List<int> = [1, 2, 3]
let counts: Map<string, int> = {"beans": 2}
```

Classes never use field literals or short `{}` initialization. Build them with
`new Class(...)` so every construction path goes through `init`.

## Types

Primitives (all unboxed in codegen):

- `int` (64-bit), `i8 i16 i32 i64`, `u8 u16 u32 u64`
- `float` (= `f64`), `f32 f64`
- `decimal` — base-10 exact number for money. See below.
- `bool`, `string` (immutable), `byte` (= `u8`)

### decimal

- Exact base-10 math: `0.1 + 0.2 == 0.3`. Always. Float can't do that.
- Use it for every money value. Using `float` for money should feel wrong in beans.
- 128-bit value type (C#-style, ~28 significant digits) — fast, stack-allocated, no heap.
- Native locals, fields, parameters, returns, and arithmetic use one inline LLVM `i128`.
  Typed generic storage keeps decimal inline in List, Map values, Box, Arena,
  Shared, Mutex, Channel, and thread results. A narrow Option/Result keeps its
  existing representation.

### Number rules

- A number literal takes the type the spot demands: `let p: decimal = 19.99` makes a decimal, `let f: f64 = 19.99` makes a float. No suffix zoo.
- With no demand, an integer literal is `int` and a decimal-point literal is `f64`.
- **No implicit numeric conversions, ever.** Mixing `int`/`float`/`decimal` needs `as`: `price * (qty as decimal)`.
- Integer literals must fit their demanded type. The checker rejects both ends outside the exact `i8`..`u64` range.
- Fixed-width integer `+`, `-`, `*`, unary `-`, and bit operations wrap to that width. Shift counts are masked by `width - 1`. Divide or modulo by zero panics.
- Integer casts keep the low target-width bits. Widening sign-extends a signed source and zero-extends an unsigned source.
- `f32` rounds after every literal, cast, and arithmetic operation. It is a real 32-bit LLVM value in locals, calls, and fields, not an alias for `f64`.

The native backend uses exact LLVM integer, float, and decimal types for locals,
parameters, returns, arithmetic, and packed class fields. List keeps its old
data/len/cap prefix for hot scalar code, but wide structs, fixed arrays, SIMD,
slices, and inline Option/Result values use their real stride plus an ARC pointer
mask, including inline 16-byte decimals. Map keeps its existing one-slot key and
narrow-value path, while wide values use a parallel typed-width buffer with the
same ARC mask. Box, Arena, Shared, Mutex, and Channel also use typed-width
storage, as do thread results. User enums use aligned inline layout for wide
payloads inside their ARC object. A stored wide Map key gets one immutable ARC
box; lookup keys stay on the stack and generated equality/hash functions walk
their fields rather than padding bytes.

### Collections

```
var xs: List<int> = [1, 2, 3]
var m: Map<string, int> = {"a": 1, "b": 2}
var ordered: OrderedMap<string, int> = {}
xs.push(4)
let n: Option<int> = m.get("a")     // no null, no panic
```

**List methods (v0.5, implemented):** `clone`, `push`, `reserve(capacity)`,
`pop`/`first`/`last`/`get(i)` → `Option<T>`,
`len`, `max`/`min` → `Option<T>` (ordered elements: numbers, strings, bools — or a generic
param, trusting its constraint), `contains`, `index_of` → `Option<int>`, `insert(i, v)` and
`remove(i) -> T` (panic out of range), `reverse`, `clear`, `slice(from, to)` (copy, half-open,
panics), `sort` (ordered elements), `sort_by(fn(a: T, b: T) -> bool)` (any `T`; the predicate
is strict less-than), `sort_by_key(fn(T) -> int)` (one key call per item), `join(sep)`.
Sorts are **stable**. The native backend uses a stable radix path for integers and integer
keys, and the shared merge semantics for other values and custom predicates.

**Map and OrderedMap methods (v0.5, implemented):** `clone`, `get` → `Option<V>`,
`set` (also `m[k] = v` sugar), `insert(k, v) -> bool` (false leaves the old value),
`reserve(capacity)`,
`len`, `contains`, `remove(k) -> bool`, `keys` → `List<K>`, `values` → `List<V>`, `clear`.
`Map` makes no iteration-order promise. `OrderedMap` promises insertion order;
updating a key keeps its place, while removing and reinserting it moves it to the
end. Lookup is hash-indexed (O(1)) in both backends, and `remove` is amortized
O(1). Plain Map swap-removes entries, so deletion may change enumeration order;
OrderedMap keeps stable entry slots and compacts holes as needed.

Bracket reads are checked, required reads: `list[i]` panics when the index is
outside the list, and `map[key]` panics when the key is missing. Use
`list.get(i)` or `map.get(key)` when absence is expected; both return `Option`.
Bracket assignment stays `list[i] = value` and `map[key] = value`.

Map values may be wide structs, fixed arrays, SIMD vectors, slices, inline
Option/Result values, or decimals. Their nested ARC fields are retained, dropped,
cloned, returned by `get`, and copied into `values()` recursively. Struct,
fixed-array, and inline Option/Result keys work when their contents satisfy
`Eq` and `Hash`; `keys()` returns their real value layout. Stored keys own nested
ARC fields, while `get`, `contains`, and `remove` use allocation-free stack keys.

List, Map, and OrderedMap are move-only unique-buffer values. Binding,
assignment, storage, and return use `move`; function parameters and loop reads
borrow by default. `clone()` makes an independent collection buffer, so changing
the clone does not change the original. Class elements remain shared ARC
references. A collection holding another move-only value cannot be cloned yet,
because that needs a type-specialized deep clone. Likewise, `get`/index reads
cannot copy a move-only element; List `pop`/`remove` are consuming reads.

Everything has methods:

```
(-5).abs()          // 5
"42".to_int()       // Result<int>
3.7.round()         // 4
xs.len()
```

## Functions

```
fn add(a: int, b: int) -> int {
    return a + b
}

pub fn log_line(msg: string) {      // no -> means no return value
    io.println(msg)
}
```

**There is no implicit tail return.** A trailing expression is a statement like any other — its
value is discarded, not returned. A function with a `->` must say `return`, on every path:

```
fn wrong() -> int {
    var sum: int = 0
    if flag() { sum = 1 }
    sum                             // error: 'wrong' must return int
}
```

The checker rejects a body that can finish without returning. A path counts as returning if it
ends in `return`, an `if`/`else` where both sides return, a statement `match` whose arms all
return, or a `for { }` with no `break` (which never finishes at all).

### Anonymous functions

`fn` without a name is a closure. It captures the variables around it. `fn(int) -> int` is also the type of a function.

```
let double: fn(int) -> int = fn(x: int) -> int { return x * 2 }
xs.map(fn(x: int) -> int { return x * 2 })
```

## Classes

```
class User {
    name: string
    age: int = 0            // default value
    pub email: string       // fields private to package unless pub

    pub fn init(name: string) {
        self.name = name
    }

    fn greet() -> string {
        return "hi {self.name}"
    }

    static fn guest() -> User {
        return new User("guest")
    }
}

let u: User = new User("jul")
```

- Methods are instance methods by default. Their `self` binding is implicit and
  available in the body; it is never written in the parameter list.
- `static fn` is required for class statics. A static method has no `self` and is
  not inherited.
- `new Class(...)` is the only class-construction form and always follows the
  class's `init` rules. Class field literals and plain `Class(...)` calls are errors.
- Named statics remain for fallible or non-construction operations, including
  `File.open`, `MMap.open`, `KV.open_in`, `Bytes.from`, `RawPtr.alloc`,
  `Slice.from_raw`, and `Simd4f32.splat`, `of`, and `load`.
- Infallible constructor-like builtins use `new`: `Bytes`, `Box`, `Arena`,
  `Shared`, `Mutex`, `Channel`, and `AtomicInt`.

### init and deinit (v0.7, implemented)

`init` is the constructor body. `new Class(...)` allocates the object and invokes
it. Like every instance method, `init` has implicit `self`.

```
class Conn {
    host: string
    hits: int = 0

    pub fn init(host: string) {
        self.host = host
    }

    fn deinit() {
        io.println("closing {self.host}")
    }
}

let c: Conn = new Conn("db1")
```

- `init` returns nothing and runs on a fresh object: fields with defaults start at them, the
  rest start unassigned. **Until every field is assigned, the body is a straight-line prefix**:
  each statement either assigns a field (`self.f = ...`) or touches `self` only by reading
  fields already assigned — no method calls, no passing `self` on, no `return`, and no string
  interpolation (its pieces are checked too late to prove them safe). The checker proves all
  of it, so a half-built object can never escape. After the last field, anything goes.
- A class whose fields all have defaults receives an implicit zero-argument
  initializer. A class with any required field must declare `init`.
- Construction that can fail stays a named static, such as
  `static fn open(...) -> Result<Conn>`; it may call `new Conn(...)` after validation.
- Generic classes take type arguments from the declared spot or an explicit
  constructor type: `let a: Stack<int> = new Stack()` or `new Stack<int>()`.
- `pub fn init` is what lets another package write `new Conn(...)` — the usual visibility rule.

**init and inheritance** work through `super.init(...)`, in Swift's order — own fields first:

```
class Dog extends Animal {
    breed: string
    fn init(breed: string, name: string) {
        self.breed = breed        // 1. this class's own fields
        super.init(name)          // 2. the parent's constructor, exactly once
        self.bark()               // 3. everything is assigned — anything goes
    }
}
```

- The order is what makes construction safe, not taste: a parent's init may call a method the
  subclass overrides, and by then the subclass's fields are already assigned. No vtable
  switching, no half-built reads — the checker just proves the order.
- Before `super.init`, parent fields don't exist yet — not even defaulted ones (the parent's
  init may be about to overwrite them). Assigning one is an error; `super.init` owns them.
- `super.init` runs exactly once, as a top-level statement, only inside `init`, and it is
  mandatory whenever a class above declares an init. `return` before it is an error.
- `super` is contextual: the checker recognizes only the exact form
  `super.init(...)`. General `super.method()` calls: later.
- A subclass whose added fields all have defaults inherits the nearest ancestor
  initializer — `new Pup(args)` runs it on a Pup. A subclass that adds a required
  field must declare its own init.
- A class whose parent has *no* init may still declare one; its prefix then covers the
  inherited fields too, under normal field visibility rules.

`deinit` is the destructor. It runs exactly once, on whichever thread drops the last
reference, the moment the count hits zero — and before the fields are released, so the body
can still read them. Deterministic, like C++/Swift: no GC pause, no "sometime later".

- No parameters, no return value, never called by hand: construction calls `init`, death
  calls `deinit`.
- A subclass `deinit` runs first, then its parent's, automatically — no `override`, ever.
- `self` must not escape a `deinit`. The object is being destroyed; storing `self` anywhere
  is use-after-free by definition.
- A panic inside `deinit` is fatal (same rule as defer).
- An object that dies **inside a reference cycle** does not get its `deinit` — a cycle never
  drops to zero on its own, so if it owns a resource, break the cycle by hand. (`weak`
  references: later.)

## Inheritance and interfaces

Classes have one base class and may implement many interfaces. Interfaces may
extend other interfaces:

```
interface Shape {
    fn area() -> f64

    // default method bodies allowed (kills most need for abstract classes)
    fn describe() -> string {
        return "shape with area {self.area()}"
    }
}

interface NamedShape extends Shape {
    fn name() -> string
}

class Circle implements Shape {
    r: f64

    fn area() -> f64 {
        return 3.14159265 * self.r * self.r
    }
}

class LoudCircle extends Circle implements NamedShape {
    // overriding a real method requires the keyword — typo protection
    override fn describe() -> string {
        return "A CIRCLE. AREA {self.area()}."
    }
}
```

`extends` takes one class base, while `implements` takes comma-separated
interfaces. Interface requirements and default methods are instance methods.
Static interface methods are unsupported. There is no `abstract` or `final` yet.

### Downcast

`as?` checks and returns an Option — never crashes:

```
let s: Shape = pick_a_shape()
match s as? Circle {
    some(c) => io.println("circle, r = {c.r}"),
    none    => io.println("something else"),
}
```

(`as` stays for explicit numeric casts and upcasts only.)

## Enums

User-defined variants, snake_case, payloads allowed. Built for `match`:

```
enum Status {
    active
    suspended
    closed
}

enum Payment {
    cash
    card(number: string)
    transfer(iban: string, amount: decimal)
}

fn describe(p: Payment) -> string {
    return match p {
        cash => "cash",
        card(n) => "card ending {n.last(4)}",
        transfer(iban, amt) => "sent {amt} to {iban}",
    }
}
```

Enums are objects too — they can carry methods (`fn label() -> string { ... }`
inside the enum body, with implicit `self`).

## Option and Result

Core rule: **a function that can fail says so in its return type.**

```
fn find(users: List<User>, name: string) -> Option<User> {
    for u: User in users {
        if u.name == name {
            return some(u)
        }
    }
    return none
}

fn parse_age(s: string) -> Result<int> {
    let n: int = s.to_int()?         // ? = if err, return it up. else unwrap.
    if n < 0 {
        return err("negative age")
    }
    return ok(n)
}
```

- `some none ok err` are prelude names (enum variants), **not keywords**.
- `Result<T>` means `Result<T, Error>` — `Error` is a built-in class (msg, kind, cause). Custom error types via `Result<T, MyError>`.
- `?` propagates. `match` handles. Helpers for the rest:

```
match parse_age(input) {
    ok(n)  => io.println("age {n}"),
    err(e) => io.println("bad: {e.msg}"),
}

let age: int = parse_age(input).or(18)                    // fallback
let u: User = find(users, "jul").expect("must exist")     // crash with message

let adult: Option<User> = find(users, "jul").filter(fn(u: User) -> bool {
    return u.age >= 18
})
let label: Option<string> = adult.map(fn(u: User) -> string {
    return u.name
})
let parsed: Result<int> = load_text().and_then(fn(s: string) -> Result<int> {
    return s.to_int()
})
let count: int = parsed.recover(fn(e: Error) -> int { return 0 })
```

`Option` has `map`, `and_then`, and `filter`. `Result` has `map`,
`and_then`, and `recover`. They are instance methods on the value, not functions
in `std.option` or `std.result`. These methods copy the active input payload, so
its type must implement `Clone`. Their inline, boxed, and null-niche layouts do
not change.

Native `Option` uses three layouts without changing source semantics: pointer
payloads use null as `none`, wide inline values such as structs, fixed arrays,
SIMD vectors, slices, and nested wide values use `{has_value, payload}`, and
small scalar payloads keep the boxed enum ABI. A `Result` with either wide
payload uses `{is_error, ok_payload, error_payload}`; its inactive payload is
zero. The compiler retains and drops references nested inside these aggregates.
Wide Options and Results pass and return by value and do not allocate their own
enum box. List and Map values can store them inline, including nested ARC fields.
Box, Arena, Shared, Mutex, Channel, and thread results can store them inline too.
User enums remain ARC values, but wide payloads use their real aligned layout
inside the enum object and participate in matching, equality, hashing, and ARC.

## Control flow

One loop keyword, three shapes:

```
for { }                        // forever
for x < 10 { }                 // while
for i: int in 0..10 { }        // range, exclusive. 0..=10 inclusive
for u: User in users { }       // any iterable
```

`break`, `continue`, `return` as usual. No `do-while`, no `switch`, no ternary, no `++`/`--`.

### if and match as values

```
let grade: string = if score >= 90 { "a" } else { "b" }
```

No `return` in there — and that's on purpose, not an inconsistency. `return` always means exactly one thing in beans: *leave the function*. If that branch said `return "a"`, it would exit the whole function, not produce a value. So the rule is:

- **Statement position:** branches hold statements, `return` works as usual.
- **Value position:** each branch is exactly one expression, and that expression is the value. It's a ternary that reads like an if. Need multiple statements in a branch? Use a `var` and the statement form.

`match` works the same way: `pattern => expression` in value position, and arms can pattern-match on values, variants, ranges, `_`:

```
match code {
    200        => "ok",
    301 | 302  => "moved",
    400..=499  => "client bug",
    _          => "who knows",
}
```

**Statement position** additionally allows block arms — several statements, no value (v0.4):

```
match ch.recv() {
    some(v) => {
        total += v
        io.println("got {v}")
    }
    none => { break }
}
```

The `{` must follow `=>` on the same line. A block arm in value position is an error — same rule as if. (Corner case: a map literal as an arm *value* needs parens, `x => ({"a": 1})`.)

## Generics

Monomorphized (a real copy per type, like C++ templates — this is a speed feature):

```
class Stack<T> {
    items: List<T> = []

    fn push(x: T) { self.items.push(x) }
    fn pop() -> Option<T> { return self.items.pop() }
}

fn largest<T implements Order>(xs: List<T>) -> Option<T> { ... }
fn index<K implements Eq & Hash, V>(key: K, value: V) -> Map<K, V> { ... }
```

The compiler-known interfaces are `Clone`, `Eq`, `Hash`, `Order`, `Send`, and `Sync`.
Bounds are checked when a generic function or type is used, and generic bodies
can only use operations promised by their bounds. `Order` also promises `Eq`.
User interfaces, including imported interfaces, may also be bounds. Generic
code may call the instance methods promised by those interfaces.

`Map<K, V>` and `OrderedMap<K, V>` require `K implements Eq & Hash`. Collection
`clone()` is available only when every stored type is `Clone`; ordering and
equality methods likewise require `Order` or `Eq`. Unknown interfaces are errors,
not ignored notes.

## Concurrency

Direction: **OS threads, not green threads.** Reason: green threads make every
C/C++ call expensive (Go's cgo problem — stack switching at the boundary).
Beans lives on C++ interop and wants to write databases, so real threads it is.
Closures plus `std.thread` do the whole job.

```
import std.thread

// spawn: run a closure on another thread
let t: Thread<int> = thread.spawn(fn() -> int {
    return heavy_work()
})
let n: int = t.join()               // wait + get the value

// mutex wraps the data itself — no way to touch it without holding the lock
let ledger: Mutex<Ledger> = new Mutex(new Ledger())
ledger.with(fn(l: Ledger) {
    l.post(entry)                   // locked for exactly this block, auto-unlock
})

// channels move work between threads
let ch: Channel<string> = new Channel(64)      // buffered
ch.send("job")
let job: Option<string> = ch.recv()            // none when closed and empty

// atomics for plain counters
let hits: AtomicInt = new AtomicInt(0)
hits.add(1)
```

- `Mutex<T>` holds the value inside it — `with` locks, runs your closure, unlocks on any exit path. No forgotten unlocks.
- A `thread.spawn` closure may capture only `Send` values and must return a
  `Send` value. Plain class references, List, Map, Box, Arena, Bytes, File, and
  MMap are non-`Send`. Scalars, immutable strings, AtomicInt, Mutex, a Channel
  of `Send` values, and `Shared<T>`/`Weak<T>` where
  `T implements Send & Sync` can cross.
  This makes `class` a local ARC reference by default; wrap shared mutable data
  in Mutex instead of silently racing it.

## Misc

- A `File` or `MMap` closed while worker threads are live keeps its OS fd/mapping open until
  the handle's last reference drops, then releases it. This stops a racing op on another thread
  from hitting an fd number that `close()` freed and the OS reused for a different file. The
  logical `closed` flag flips immediately, so same-thread `close()` semantics are unchanged; only
  the OS-level release is deferred, and only while threads run.
- `defer f.close()` — runs when the function exits normally, including through
  `return` and `?`, newest first and before local destruction. Must sit at the
  top level of the function body (not inside `if`/`for`/blocks — it is a function-exit hook,
  and nested registration would need runtime capture the native backend does not do). A panic
  exits the process without running defers, and a panic inside a defer is itself fatal.
  (Go's best idea, minus unwinding.)
- `unsafe { }` — gates low-level operations. The first implemented part is
  `RawPtr<T>` for primitive integer, float, bool, raw-pointer, fixed-array, and
  declared `extern "C" struct`/`union` values. These shapes can nest.
  `RawPtr.alloc(n)`
  allocates zeroed unmanaged storage; `null()` and `from_address(u64)` create
  pointer values. `read`, `write`, `read_volatile`, `write_volatile`, `offset`,
  `address`, `is_null`, `element_size`, `element_align`, overlap-safe
  `copy_from`, zeroing `fill_zero`, and `free` are only legal inside an unsafe
  block. The
  volatile forms lower to LLVM volatile memory operations for device/shared
  memory; they are not atomic. Integer and bool pointers also provide
  sequentially consistent `atomic_load`, `atomic_store`, and
  `atomic_compare_exchange`; integer pointers add `atomic_fetch_add`, which
  returns the old value. A null memory operation gives a runtime
  panic, but lifetime, bounds, alignment, address validity, and one matching
  `free` are the programmer's job. Atomic access checks its natural alignment;
  ordinary and volatile access do not. Raw pointers are copyable, so freeing one
  alias leaves every other alias dangling.
- `extern "C" fn name(args) -> T` declares an unmangled host C symbol. Calls
  require `unsafe {}`. The ABI supports up to six integer, bool, `RawPtr`,
  `f32`, `f64`, or `extern "C" struct`/`union` arguments and the same return
  types (or no return). Aggregates may contain nested C-layout records and
  fixed arrays. Clang owns the platform ABI lowering: native builds link a
  generated pointer-ABI wrapper, and the interpreter compiles and caches a tiny
  trampoline for each bridged signature. A parameter may be a C callback such
  as `fn(i32, i32) -> i32`; its arguments and return use the same C-safe type
  set and may include C-layout records. Beans closures and stored top-level
  functions both work. The callback is borrowed, synchronous, and same-thread:
  C may call it only before the surrounding extern call returns, must not store
  it, and must not invoke it from another thread. Use a separate native entry
  point plus `Shared`/`Mutex` for longer-lived or cross-thread callbacks.
- `Simd4f32` is an inline four-lane `f32` vector, available inside `unsafe`.
  `splat(x)` and `of(a, b, c, d)` construct it; `+`, `-`, `*`, and `/` are
  lane-wise; `lane(i)` and `sum()` read it; `load(RawPtr<f32>)` and
  `store(RawPtr<f32>)` move 16 bytes without requiring aligned memory. Native
  code uses LLVM vector operations. SIMD values can be carried by inline
  `Option` and `Result` and stored directly in List, Map values, Box, Arena,
  Shared, Mutex, Channel, and thread results. SIMD does not implement `Hash`,
  so it cannot be a Map key.
- `[T; N]` is a fixed-size inline array. It accepts inline scalar, `RawPtr`,
  nested fixed-array, and struct elements with `1 <= N <= 4096`. A list-shaped
  literal gets fixed-array meaning from its declared spot:
  `var lanes: [f32; 4] = [1, 2, 3, 4]`. Arrays copy by value, pass and return
  inline, support checked integer indexing, element assignment on `var`
  locals, `len()`, equality, and `for` iteration. List, Box, and Arena store
  arrays inline; Map stores them inline as values and as structural keys when
  their element type implements `Hash`. Shared, Mutex, Channel, and thread
  results use the same typed-width layout.
- `Slice<T>` is a non-owning inline `{pointer, length}` view for the raw-memory
  element set above. `Slice.from_raw(ptr, len)`,
  `get`, `set`, indexing, `subslice`,
  `as_ptr`, and iteration require `unsafe`; reads and writes are bounds checked.
  A non-empty slice rejects a null pointer. The caller must keep the backing
  allocation alive and must not use the view after `free`.

- `struct` declares an inline value type. It copies by value and is passed and
  returned as an LLVM aggregate, with no ARC header or heap allocation:

  ```beans
  extern "C" struct Packet {
      tag: u8
      count: u32
      ratio: f32
  }
  ```

  `extern "C"` fixes declaration order and the target C size/alignment rules, so
  `RawPtr<Packet>` and `Slice<Packet>` can access matching native memory.
  Fields are private unless marked `pub`, as with classes. Ordinary structs can
  own strings, classes, collections, Options/Results, and other ARC values; the
  compiler retains and drops those fields recursively through copies, arrays,
  class fields, and typed List, Map-value, Box, and Arena storage. `extern "C"` structs stay restricted to
  inline scalars, `RawPtr`, fixed arrays, and nested C-layout structs so their C
  ABI has no hidden ownership policy. A direct or array-wrapped
  recursive value edge is rejected because it has no finite size; use `RawPtr`
  or `Box` for that edge. Generic structs, inheritance, methods, ARC reference
  fields in C-layout records remain open. Ordinary structs that satisfy `Eq`
  and `Hash` can be Map keys; stored keys use an immutable compiler-owned box
  and lookups use a stack copy. A field can be changed only through a `var`
  local.

- `extern "C" union` declares overlapping inline scalar, `RawPtr`, fixed-array,
  or nested C-layout storage. It must be
  initialized with exactly one named field. Initialization, reads, and writes
  require `unsafe`, because Beans does not track which member is active:

  ```beans
  extern "C" union Word {
      bits: u32
      number: f32
  }
  ```

  Union values copy, pass, return, and round-trip through `RawPtr` inline.
  Fields have C size/alignment and all start at offset zero. This first slice
  has no defaults, methods, generics, inheritance, compound field assignment,
  ARC reference fields, or direct old-container storage.

`unique` is a contextual declaration modifier; `extern "C"` is a declaration
modifier built from the `extern` keyword. Standard modifier
order is `pub unique class` and `pub extern "C" struct`.

## Keywords and modifiers

```
class struct union interface enum fn let var pub override
if else for in match return break continue move inout
import as defer unsafe extern new extends implements static
self true false unique
```

`some none ok err` are ordinary names. `super` is contextual. `spawn` is a
library function, not a keyword.

## Decided

- Syntax v0.7 (implemented): `new Class(...)` is the only class construction;
  implicit instance `self`; explicit `static fn`; `extends`/`implements`;
  `T implements A & B`; `move`; `unique class`; `extern "C" struct/union`;
  Option/Result combinators are instance methods; old forms have no aliases
- `init`/`deinit`: constructor and destructor bodies use implicit `self`;
  all-default classes get an implicit initializer; required fields require
  `init`; subclass initializer inheritance is allowed when added fields all
  have defaults; `super.init` keeps the Swift order — own fields, then parent,
  then full self; destruction runs at refcount zero before field release,
  subclass then parent, and is skipped for cycle garbage
- Stdlib v0.5 phase 4 (implemented): Beans-written `std.reader` line reading over positional I/O (the old native `BufReader` is gone), format specs in interpolation (`{x:8.2}` — first top-level `:` in the braces; the same rendering as `std.fmt`), `chars()` for UTF-8, varint + crc32 on `Bytes`, `MMap.resize` (the handle keeps its fd), `Dir.walk` (recursive, sorted, relative), and Beans-written `std.path`
- Stdlib v0.5 phase 3 (implemented): the List/Map method set with **stable** sorts (`sort_by` takes a less-than closure; both backends run the identical merge), `Bytes` value `==`, advisory file locks, `MMap` (whole-file, shared, drop unmaps, grow = close + reopen), `std.fmt`, and printing widened to enums and lists — `variant(payload)` / `[a, b]` — everywhere strings interpolate; maps, class instances, and `Result` stay unprintable
- Stdlib v0.5: the string method set, `Bytes`, `File`/`Dir`, `std.os`, and the `std.io` console set (implemented); byte semantics, panics carry positions, mutators return self for chaining, fs errors carry kind slugs
- Modules: `beans.mod`, one folder = one package, git imports with a global cache (v0.4, implemented)
- Block-bodied match arms in statement position (v0.4, implemented)
- `pub interface` exposes its method set implicitly (v0.4)
- Explicit types everywhere, no inference (v0.2) — match bindings relaxed in v0.3
- Named field literals remain for structs; classes construct only with `new`
- No `+` on strings — interpolation / `std.fmt` / `join` only (v0.3)
- Private by default everywhere, `pub` to expose (confirmed v0.3)
- OS threads + checked `Send` captures/returns + `Mutex<T>.with` + `Channel<T>`
- `decimal` built-in for money (v0.2)
- Go-style remote imports from git hosts + beans.mod (v0.2)
- `Result<T>`, error type defaults to built-in `Error`
- User-defined enums in v1, payloads allowed
- Java-style method mutability (no `mut self`)
- `as?` checked downcast returning `Option<T>`
- `fn`

## Open questions

1. decimal division rounding: default mode (banker's rounding?) and a
   `.round(places, mode)` API — settle when we do the stdlib.
