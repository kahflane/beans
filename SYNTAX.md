# beans syntax — draft 0.4

Language: **beans** · extension: **.b** · status: draft for discussion
Toolchain status: **lexer + parser + loader + checker + interpreter + native backend implemented** — including multi-file modules and git imports

## What beans is for

Two target jobs, and the design serves both:

1. **Business apps** (accounting, ERP, billing) → mandatory explicit types, `decimal` money math, no null, no exceptions, boring and readable.
2. **Systems work** (databases, OS/hardware control) → sized ints, value types, `unsafe` layer with raw memory and C/C++ interop, no GC pauses.

## Design rules

1. Small grammar. Budget: 23 keywords (Go has 25, Java 50+, C++ 90+).
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
- **There is no `+` for strings.** Building strings happens through interpolation, `std.fmt` (sprintf-style: padding, precision, alignment), or `list.join(sep)`. One way to do it, and it's the readable one.
- Escapes: `\n \t \r \0 \\ \" \{ \}`.

## Variables

```
let x: int = 5              // can't be reassigned (like Java final)
var total: decimal = 0.0    // can be reassigned
```

`let` means the *variable* can't be rebound. The object it points to can still change inside (Java-style — no borrow checker, no `mut` markers).

### Short init

When the left side already states the type, the right side can drop it:

```
var st: Stack<int> = {}                  // same as Stack<int> {}
let u: User = { name: "jul", age: 30 }   // same as User { ... }
```

Works in variable declarations and field defaults.

## Types

Primitives (all objects, all unboxed in codegen):

- `int` (64-bit), `i8 i16 i32 i64`, `u8 u16 u32 u64`
- `float` (= `f64`), `f32 f64`
- `decimal` — base-10 exact number for money. See below.
- `bool`, `string` (immutable), `byte` (= `u8`)

### decimal

- Exact base-10 math: `0.1 + 0.2 == 0.3`. Always. Float can't do that.
- Use it for every money value. Using `float` for money should feel wrong in beans.
- 128-bit value type (C#-style, ~28 significant digits) — fast, stack-allocated, no heap.

### Number rules

- A number literal takes the type the spot demands: `let p: decimal = 19.99` makes a decimal, `let f: f64 = 19.99` makes a float. No suffix zoo.
- With no demand, an integer literal is `int` and a decimal-point literal is `f64`.
- **No implicit numeric conversions, ever.** Mixing `int`/`float`/`decimal` needs `as`: `price * (qty as decimal)`.

### Collections

```
var xs: List<int> = [1, 2, 3]
var m: Map<string, int> = {"a": 1, "b": 2}
xs.push(4)
let n: Option<int> = m.get("a")     // no null, no panic
```

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

fn largest<T: Comparable>(xs: List<T>) -> Option<T> { ... }
```

## Concurrency (sketch — settle before parser work on it)

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

- `Mutex<T>` holds the value inside it (Rust's good idea) — `with` locks, runs your closure, unlocks on any exit path. No forgotten unlocks.
- Honesty note: v1 is Java-level here — the compiler won't stop a determined data race. `Mutex`/`Channel`/`Atomic` are the blessed path. Compile-time race checks: maybe later.

## Misc

- `defer f.close()` — runs when the function exits, any path. (Go's best idea.)
- `unsafe { }` — reserved for the low-level layer: raw pointers, manual memory, C/C++ interop, syscalls. This is the databases/OS story. Separate doc later.

## Keywords (23)

```
class interface enum fn let var pub override
if else for in match return break continue
import as defer unsafe self true false
```

`some none ok err new` are ordinary names, not keywords. `spawn` is a library function, not a keyword.

## Decided

- Modules: `beans.mod`, one folder = one package, git imports with a global cache (v0.4, implemented)
- Block-bodied match arms in statement position (v0.4, implemented)
- `pub interface` exposes its method set implicitly (v0.4)
- Explicit types everywhere, no inference (v0.2) — match bindings relaxed in v0.3
- Short init `= {}` when the left side states the type (v0.3)
- No `+` on strings — interpolation / `std.fmt` / `join` only (v0.3)
- Private by default everywhere, `pub` to expose (confirmed v0.3)
- OS threads + `thread.spawn` closure + `Mutex<T>.with` + `Channel<T>`, no new keywords (v0.3, sketch)
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
