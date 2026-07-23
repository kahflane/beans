# beans syntax — draft 0.4

Language: **beans** · extension: **.b** · status: draft for discussion
Toolchain status: **lexer + parser + loader + checker + interpreter + native backend implemented** — including multi-file modules and git imports

## What beans is for

Two target jobs, and the design serves both:

1. **Business apps** (accounting, ERP, billing) → mandatory explicit types, `decimal` money math, no null, no exceptions, boring and readable.
2. **Systems work** (databases, OS/hardware control) → sized ints, value types, `unsafe` layer with raw memory and C/C++ interop, no GC pauses.

## Design rules

1. Small grammar. Keep the keyword set small; it is currently 26.
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
- Cross-package access: `util.some_fn()`, `util.User`, `util.User.new(...)`, `util.color.red` — anything `pub`. Methods of a `pub interface` travel with it (an interface is its method set).
- `pub` is enforced in initializers too: `util.User { hidden: 1 }` is an error unless `hidden` is `pub`. A class whose non-`pub` field has no default can only be built inside its own package.
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
`count_chars(from, to)` for a checked, allocation-free byte range scan.

## Bytes (v0.5, implemented)

The binary buffer — strings stay text; anything binary is `Bytes`. Mutating methods return
self, so page-building chains work: `Bytes.new(4096).put_u32(0, root).put_u64(8, lsn)`.

- `Bytes.new(n)` (zeroed, panics on negative), `Bytes.from(s)` (copies the text bytes)
- `len()`, `reserve(n)`, `resize(n)` (regrown range reads zero), `fill(v)`
- `get(i)` / `set(i, v)` — one byte, panics out of range
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
- **std.reader**: `reader.Reader(f)` then `read_line()` → `Result<Option<string>>` —
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
the generic `count`, `filter`, `transform`, and `unique_of` functions.
`std.option` provides generic `map`, `and_then`, and `filter`; `std.result`
provides generic `map`, `and_then`, and `recover`; `std.math`
provides `clamp_int` and `gcd`; `std.bytes` provides Beans-written `crc32`,
`varint_size`, `encode_varint`, and checked `decode_varint`; `std.path` is fully
Beans-written; `std.fmt` implements `hex`, `bin`, and `group` in Beans; and
`std.fs` implements the high-level whole-file byte/write/copy helpers in Beans;
`std.reader` implements buffered line reading in Beans.
Floating-point/decimal conversion and guarded padding remain native. These go through
the same checker, interpreter, MIR, and native backend as user code.
`BEANS_STDLIB` can point the loader at another shipped-library root. Native
registry rows remain for low-level allocation/storage, raw bytes, OS calls,
atomics, and thread entry while more of `core` and `std` move to `.b` files.
  (none at EOF), `read_all()`.

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

`take name` moves the value out of a local binding. The old binding cannot be
read again unless it is a `var` and gets a new value first:

```
var job: Job = next_job()
let running: Job = take job
job = next_job()                 // reinitializes it
```

The checker rejects use after move and a value moved on only one branch. A
move on every branch is definite. Normal parameters, loop variables, match
bindings, and closure captures are borrowed, so they cannot be taken. Moving an outer
local from a loop is also rejected because the next iteration would see an
empty binding. For now `take` names a whole local; field and index moves need
consuming accessors such as List `remove`.

Parameters borrow by default. A `take` parameter owns its argument and drops it
at function exit unless the body moves it onward:

```
fn enqueue(take jobs: List<Job>) { ... }

var batch: List<Job> = make_batch()
enqueue(take batch)
```

A fresh result can be passed directly; an existing move-only local needs
`take`. Take modes must match across interface methods and overrides. Function
values and closures do not carry ownership modes yet, so a function with take
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
values release the old value and take ownership of the replacement on overwrite.

`@move_only class` gives the same outer-handle rule to a user type:

```
@move_only class Packet {
    bytes: Bytes
}
```

It cannot be copied by binding, assignment, return, or storage. Use `take` to
move it. A subclass of a move-only class is move-only too. This controls the
reference handle; fields inside the object still follow their own rules.

`Box<T>` and `Arena<T>` are move-only outer handles. Binding or assigning an
existing handle needs `take`; function parameters borrow by default.

```
var value: Box<int> = Box.new(7)
value.set(9)
let owned: Box<int> = take value

var arena: Arena<string> = Arena.new(1024)
let handle: int = arena.put("bean")
let word: string = arena.at(handle)       // checked; panics on a bad handle
let maybe: Option<string> = arena.get(handle)
arena.clear()                             // drops all values in one pass
```

`Box.new(value)` owns one heap slot. `get()` returns the value and `set(value)`
replaces it. The native runtime uses the common iterative ownership walker, so
boxed chains do not recurse during teardown.

`Arena.new(capacity)` needs a declared `Arena<T>` type. `put(value)` appends and
returns a stable integer handle; `len`, `at`, `get`, and `clear` operate on the
current region. `clear` keeps capacity but invalidates every old handle. This
first arena stores typed 64-bit runtime slots. Inline structs and borrowed
arena references still wait for the value-layout and lifetime work.

`Shared<T>` is the explicit thread-safe shared-ownership handle. `Weak<T>`
observes the same control block without keeping the value alive:

```
let shared: Shared<string> = Shared.new("beans")
let weak: Weak<string> = shared.downgrade()
let live: Option<Shared<string>> = weak.upgrade()
let gone: bool = weak.expired()
```

`get()` returns a copy of the value. The control block owns one value reference
until its last strong handle dies; upgrade uses an atomic compare/exchange, so
it cannot revive a dead value. A cycle made through `Shared` must be broken with
`Weak`, like C++ `shared_ptr`/`weak_ptr`; the local-class cycle collector does
not trace through explicit Shared control blocks. `Send`/`Sync` trait checks are
not enforced yet, so this is the runtime ownership foundation, not the final
compile-time race model.

### Short init

When the left side already states the type, the right side can drop it:

```
var st: Stack<int> = {}                  // same as Stack<int> {}
let u: User = { name: "jul", age: 30 }   // same as User { ... }
```

Works in variable declarations and field defaults.

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
  The current one-slot generic runtime uses a small adapter box only when a decimal is
  stored in List, Map, Option/Result, Box, Arena, Mutex, Channel, or a thread result.

### Number rules

- A number literal takes the type the spot demands: `let p: decimal = 19.99` makes a decimal, `let f: f64 = 19.99` makes a float. No suffix zoo.
- With no demand, an integer literal is `int` and a decimal-point literal is `f64`.
- **No implicit numeric conversions, ever.** Mixing `int`/`float`/`decimal` needs `as`: `price * (qty as decimal)`.
- Integer literals must fit their demanded type. The checker rejects both ends outside the exact `i8`..`u64` range.
- Fixed-width integer `+`, `-`, `*`, unary `-`, and bit operations wrap to that width. Shift counts are masked by `width - 1`. Divide or modulo by zero panics.
- Integer casts keep the low target-width bits. Widening sign-extends a signed source and zero-extends an unsigned source.
- `f32` rounds after every literal, cast, and arithmetic operation. It is a real 32-bit LLVM value in locals, calls, and fields, not an alias for `f64`.

The native backend uses exact LLVM integer, float, and decimal types for locals,
parameters, returns, arithmetic, and packed class fields. List, Map, channel,
mutex, and enum payload slots still use one 64-bit runtime slot and are converted
on entry/exit; this keeps the current generic runtime ABI while the move-only
buffer layout is being built.

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

List, Map, and OrderedMap are move-only unique-buffer values. Binding,
assignment, storage, and return use `take`; function parameters and loop reads
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

    // no `self` param = static
    fn new(name: string) -> User {
        return User { name: name }     // struct-style init
    }

    // `self` param = instance method. no `static` keyword needed.
    fn greet(self) -> string {
        return "hi {self.name}"
    }
}

let u: User = User.new("jul")
```

- `ClassName { field: value }` is the raw initializer. `fn new(...)` is a convention, not a keyword.
- Want two ways to build it? Write `new` and `from_json`. No overloading magic.

### init and deinit (v0.6, implemented)

`init` is the constructor — a method literally named `init`, no new keyword. Declaring one
changes how the class is built: call the class like a function, and the raw `{ }` form becomes
an error for that class. One way in, so its invariants always hold.

```
class Conn {
    host: string
    hits: int = 0

    pub fn init(self, host: string) {
        self.host = host
    }

    fn deinit(self) {
        io.println("closing {self.host}")
    }
}

let c: Conn = Conn("db1")
```

- `init` returns nothing and runs on a fresh object: fields with defaults start at them, the
  rest start unassigned. **Until every field is assigned, the body is a straight-line prefix**:
  each statement either assigns a field (`self.f = ...`) or touches `self` only by reading
  fields already assigned — no method calls, no passing `self` on, no `return`, and no string
  interpolation (its pieces are checked too late to prove them safe). The checker proves all
  of it, so a half-built object can never escape. After the last field, anything goes.
- Construction that can fail stays a static — `fn new(...) -> Result<Conn>` validates, then
  calls `Conn(...)`.
- Generic classes take their type arguments from the spot: `let s: Stack<int> = Stack()`.
- `pub fn init` is what lets another package write `Conn(...)` — the usual visibility rule.

**init and inheritance** work through `super.init(...)`, in Swift's order — own fields first:

```
class Dog : Animal {
    breed: string
    fn init(self, breed: string, name: string) {
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
- `super` is not a keyword (still 23): the checker recognizes only the exact form
  `super.init(...)`. General `super.method()` calls: later.
- A subclass that adds **no required fields** inherits the constructor — `Pup(args)` runs the
  nearest ancestor init on a Pup. A subclass that adds required fields must declare its own
  init. Either way `{ }` stays banned while any ancestor has an init.
- A class whose parent has *no* init may still declare one; its prefix then covers the
  inherited fields too, under the raw form's visibility rules.

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

Single inheritance, many interfaces, one syntax — a colon list:

```
interface Shape {
    fn area(self) -> f64

    // default method bodies allowed (kills most need for abstract classes)
    fn describe(self) -> string {
        return "shape with area {self.area()}"
    }
}

class Circle : Shape {
    r: f64

    fn area(self) -> f64 {
        return 3.14159265 * self.r * self.r
    }
}

class LoudCircle : Circle {
    // overriding a real method requires the keyword — typo protection
    override fn describe(self) -> string {
        return "A CIRCLE. AREA {self.area()}."
    }
}
```

No `extends`, no `implements`, no `abstract`, no `static`, no `final` (for now).

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

Enums are objects too — they can carry methods (`fn label(self) -> string { ... }` inside the enum body).

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
```

Native `Option` uses three layouts without changing source semantics: pointer
payloads use null as `none`, wide inline values such as structs, fixed arrays,
SIMD vectors, slices, and nested wide values use `{has_value, payload}`, and
small scalar payloads keep the boxed enum ABI. A `Result` with either wide
payload uses `{is_error, ok_payload, error_payload}`; its inactive payload is
zero. The compiler retains and drops references nested inside these aggregates.
Wide Options and Results pass and return by value and do not allocate their own
enum box. User enums and runtime-slot containers still cannot hold wide inline
values yet.

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

    fn push(self, x: T) { self.items.push(x) }
    fn pop(self) -> Option<T> { return self.items.pop() }
}

fn largest<T: Order>(xs: List<T>) -> Option<T> { ... }
fn index<K: Eq + Hash, V>(key: K, value: V) -> Map<K, V> { ... }
```

The built-in traits are `Clone`, `Eq`, `Hash`, `Order`, `Send`, and `Sync`.
Bounds are checked when a generic function or type is used, and generic bodies
can only use operations promised by their bounds. `Order` also promises `Eq`.
The old draft name `Comparable` remains an alias for `Order`.

`Map<K, V>` and `OrderedMap<K, V>` require `K: Eq + Hash`. Collection
`clone()` is available only when every stored type is `Clone`; ordering and
equality methods likewise require `Order` or `Eq`. Unknown traits are errors,
not ignored notes.

## Concurrency

Direction: **OS threads, not green threads.** Reason: green threads make every C/C++ call expensive (Go's cgo problem — stack switching at the boundary). Beans lives on C++ interop and wants to write databases, so real threads it is. And no new keywords — closures plus `std.thread` do the whole job, grammar stays at 23.

```
import std.thread

// spawn: run a closure on another thread
let t: Thread<int> = thread.spawn(fn() -> int {
    return heavy_work()
})
let n: int = t.join()               // wait + get the value

// mutex wraps the data itself — no way to touch it without holding the lock
let ledger: Mutex<Ledger> = Mutex.new(Ledger.new())
ledger.with(fn(l: Ledger) {
    l.post(entry)                   // locked for exactly this block, auto-unlock
})

// channels move work between threads
let ch: Channel<string> = Channel.new(64)      // buffered
ch.send("job")
let job: Option<string> = ch.recv()            // none when closed and empty

// atomics for plain counters
let hits: AtomicInt = AtomicInt.new(0)
hits.add(1)
```

- `Mutex<T>` holds the value inside it — `with` locks, runs your closure, unlocks on any exit path. No forgotten unlocks.
- A `thread.spawn` closure may capture only `Send` values and must return a
  `Send` value. Plain class references, List, Map, Box, Arena, Bytes, File, and
  MMap are non-`Send`. Scalars, immutable strings, AtomicInt, Mutex, a Channel
  of `Send` values, and `Shared<T>`/`Weak<T>` where `T: Send + Sync` can cross.
  This makes `class` a local ARC reference by default; wrap shared mutable data
  in Mutex instead of silently racing it.

## Misc

- A `File` or `MMap` closed while worker threads are live keeps its OS fd/mapping open until
  the handle's last reference drops, then releases it. This stops a racing op on another thread
  from hitting an fd number that `close()` freed and the OS reused for a different file. The
  logical `closed` flag flips immediately, so same-thread `close()` semantics are unchanged; only
  the OS-level release is deferred, and only while threads run.
- `defer f.close()` — runs when the function exits normally, newest first. Must sit at the
  top level of the function body (not inside `if`/`for`/blocks — it is a function-exit hook,
  and nested registration would need runtime capture the native backend does not do). A panic
  exits the process without running defers, and a panic inside a defer is itself fatal.
  (Go's best idea, minus unwinding.)
- `unsafe { }` — gates low-level operations. The first implemented part is
  `RawPtr<T>` for primitive integer, float, bool, raw-pointer, fixed-array, and
  declared `@c_layout struct`/`union` values. These shapes can nest.
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
  `f32`, `f64`, or `@c_layout struct`/`union` arguments and the same return
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
  `Option` and `Result`, but cannot yet be placed directly in List, Map, or the
  other eight-byte-slot runtime containers.
- `[T; N]` is a fixed-size inline array. It accepts inline scalar, `RawPtr`,
  nested fixed-array, and struct elements with `1 <= N <= 4096`. A list-shaped
  literal gets fixed-array meaning from its declared spot:
  `var lanes: [f32; 4] = [1, 2, 3, 4]`. Arrays copy by value, pass and return
  inline, support checked integer indexing, element assignment on `var`
  locals, `len()`, equality, and `for` iteration. Direct storage in the old
  eight-byte runtime containers waits for inline generic storage.
- `Slice<T>` is a non-owning inline `{pointer, length}` view for the raw-memory
  element set above. `Slice.from_raw(ptr, len)`,
  `get`, `set`, indexing, `subslice`,
  `as_ptr`, and iteration require `unsafe`; reads and writes are bounds checked.
  A non-empty slice rejects a null pointer. The caller must keep the backing
  allocation alive and must not use the view after `free`.

- `struct` declares an inline value type. It copies by value and is passed and
  returned as an LLVM aggregate, with no ARC header or heap allocation:

  ```beans
  @c_layout
  struct Packet {
      tag: u8
      count: u32
      ratio: f32
  }
  ```

  `@c_layout` fixes declaration order and the target C size/alignment rules, so
  `RawPtr<Packet>` and `Slice<Packet>` can access matching native memory.
  Fields are private unless marked `pub`, as with classes. Fields can contain
  inline scalars, `RawPtr`, fixed arrays, and nested structs; every nested value
  inside a `@c_layout` record must also have C layout. A direct or array-wrapped
  recursive value edge is rejected because it has no finite size; use `RawPtr`
  or `Box` for that edge. Generic structs, inheritance, methods, ARC reference
  fields, and old runtime-slot container storage remain open. A field can be
  changed only through a `var` local.

- `@c_layout union` declares overlapping inline scalar, `RawPtr`, fixed-array,
  or nested C-layout storage. It must be
  initialized with exactly one named field. Initialization, reads, and writes
  require `unsafe`, because Beans does not track which member is active:

  ```beans
  @c_layout
  union Word {
      bits: u32
      number: f32
  }
  ```

  Union values copy, pass, return, and round-trip through `RawPtr` inline.
  Fields have C size/alignment and all start at offset zero. This first slice
  has no defaults, methods, generics, inheritance, compound field assignment,
  ARC reference fields, or direct old-container storage.

## Keywords (28)

```
class struct union interface enum fn let var pub override
if else for in match return break continue take inout
import as defer unsafe extern self true false
```

`some none ok err new` are ordinary names, not keywords. `spawn` is a library function, not a keyword.

## Decided

- `super.init` v0.6 (implemented): Swift order — own fields, then the parent's constructor,
  then full self; exactly once, top level, mandatory when an ancestor has init; `super` is
  contextual, not a keyword; a subclass adding no required fields inherits the constructor
- `init`/`deinit` v0.6 (implemented): constructor by method name, `ClassName(args)` call form,
  straight-line-prefix field assignment proof, no init with inheritance yet; destructor runs at
  refcount zero before fields release, subclass-then-parent chain, skipped for cycle garbage,
  panic inside it fatal, `self` must not escape
- Stdlib v0.5 phase 4 (implemented): Beans-written `std.reader` line reading over positional I/O (the old native `BufReader` is gone), format specs in interpolation (`{x:8.2}` — first top-level `:` in the braces; the same rendering as `std.fmt`), `chars()` for UTF-8, varint + crc32 on `Bytes`, `MMap.resize` (the handle keeps its fd), `Dir.walk` (recursive, sorted, relative), and Beans-written `std.path`
- Stdlib v0.5 phase 3 (implemented): the List/Map method set with **stable** sorts (`sort_by` takes a less-than closure; both backends run the identical merge), `Bytes` value `==`, advisory file locks, `MMap` (whole-file, shared, drop unmaps, grow = close + reopen), `std.fmt`, and printing widened to enums and lists — `variant(payload)` / `[a, b]` — everywhere strings interpolate; maps, class instances, and `Result` stay unprintable
- Stdlib v0.5: the string method set, `Bytes`, `File`/`Dir`, `std.os`, and the `std.io` console set (implemented); byte semantics, panics carry positions, mutators return self for chaining, fs errors carry kind slugs
- Modules: `beans.mod`, one folder = one package, git imports with a global cache (v0.4, implemented)
- Block-bodied match arms in statement position (v0.4, implemented)
- `pub interface` exposes its method set implicitly (v0.4)
- Explicit types everywhere, no inference (v0.2) — match bindings relaxed in v0.3
- Short init `= {}` when the left side states the type (v0.3)
- No `+` on strings — interpolation / `std.fmt` / `join` only (v0.3)
- Private by default everywhere, `pub` to expose (confirmed v0.3)
- OS threads + checked `Send` captures/returns + `Mutex<T>.with` + `Channel<T>`, no new keywords
- `decimal` built-in for money (v0.2)
- Go-style remote imports from git hosts + beans.mod (v0.2)
- `Result<T>`, error type defaults to built-in `Error`
- User-defined enums in v1, payloads allowed
- Java-style method mutability (no `mut self`)
- `as?` checked downcast returning `Option<T>`
- `fn`

## Open questions

1. Concurrency sketch above — confirm the shape, or do you want `spawn { ... }` as keyword sugar (would be keyword #24)?
2. decimal division rounding: default mode (banker's rounding?) and a `.round(places, mode)` API — settle when we do the stdlib.
