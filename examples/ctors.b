// ctors.b — init and deinit, the whole contract (SYNTAX.md "init and deinit").
// Every deinit print pins down destruction order, so this file is the
// regression for the rule that both backends must die in the same order:
// frames newest-first, containers back-to-front, cascades node-then-fields.
import std.io
import std.thread

class Res {
    tag: string
    n: int = 0

    pub fn init(tag: string) {
        let t: string = tag           // locals are fine before fields
        self.tag = t
        self.n = self.n + 100         // defaults are already assigned
        io.println("open {self.tag}({self.n})")
    }

    fn deinit() {
        io.println("close {self.tag}")
    }

    fn touch() {
        self.n = self.n + 1
    }
}

// a static factory stays the fallible-construction pattern
class Port {
    num: int
    fn init(num: int) { self.num = num }
    fn deinit() { io.println("port {self.num} released") }

    static fn checked(n: int) -> Result<Port> {
        if n < 1 {
            return err("bad port {n}")
        }
        return ok(new Port(n))
    }
}

// deinit chains: subclass first, then parent — no override, ever
class Parent {
    a: Res = new Res("parent-field")
    fn deinit() { io.println("parent down") }
}

class Child extends Parent {
    fn deinit() {
        if true {
            return                    // early return still chains to Parent
        }
    }
}

// generic class with both hooks
class OwnedBox<T> {
    item: T
    fn init(item: T) { self.item = item }
    fn deinit() { io.println("box down") }
}

// a cycle never reaches zero by itself, so deinit is skipped on both sides
class Ring {
    name: string
    next: Option<Ring> = none
    fn init(name: string) { self.name = name }
    fn deinit() { io.println("ring {self.name} down (acyclic only)") }
}

fn scopes_and_reassign() {
    io.println("-- scopes --")
    let first: Res = new Res("first")
    let second: Res = new Res("second")
    var v: Res = new Res("old")
    v = new Res("new")                    // "old" dies right here
    v.touch()
    io.println("frame ends")          // then: new, second, first (newest-first)
}

fn containers() {
    io.println("-- containers --")
    var xs: List<Res> = []
    xs.push(new Res("L0"))
    xs.push(new Res("L1"))
    xs.clear()                        // back to front: L1 then L0
    new Res("temp")                       // statement temp dies at statement end
    var m: Map<int, Res> = {}
    m[1] = new Res("V1")
    m[2] = new Res("V2")
    m[1] = new Res("V1b")                 // overwrite kills V1 now
    let had: bool = m.remove(2)       // V2 dies
    io.println("removed {had}")
    io.println("map ends")            // clear order at frame end: V1b last in
    let some_leaf: Option<Res> = some(new Res("wrapped"))
    io.println("option ends")         // the some-box cascade frees "wrapped"
}

fn inheritance() {
    io.println("-- inheritance --")
    let c: Child = new Child()           // Parent has no init, raw form is fine
    io.println("child built")         // child deinit, parent deinit, then field
}

fn generics() {
    io.println("-- generics --")
    let b: OwnedBox<Res> = new OwnedBox(new Res("boxed"))
    io.println("box built")           // box down, then close boxed
}

// super.init: own fields first, then the parent's constructor, then anything.
// Animal's init calls a method the child overrides — safe, because the
// child's fields are already assigned when super.init runs (the whole reason
// for the order).
class Animal {
    name: string
    fn init(name: string) {
        self.name = name
        io.println("animal init hears: {self.loud()}")
    }
    fn loud() -> string { return self.name }
    fn deinit() { io.println("animal down") }
}

class Dog extends Animal {
    breed: string
    fn init(breed: string, name: string) {
        self.breed = breed            // own field, before super.init
        super.init(name)              // parent's turn — exactly once
        io.println("dog built: {self.name} the {self.breed}")
    }
    override fn loud() -> string { return "{self.breed}!" }
    fn deinit() { io.println("dog down") }
}

// adds no required fields, so Dog's constructor is inherited
class Pup extends Dog {
    treats: int = 3
}

fn supers() {
    io.println("-- super.init --")
    let d: Dog = new Dog("corgi", "rex")
    io.println(d.loud())
    let p: Pup = new Pup("lab", "sam")    // new Pup(args) runs Dog's init
    io.println("pup {p.name} gets {p.treats}")
}                                     // deinits chain: dog down, animal down — twice

fn threads() {
    io.println("-- threads --")
    let t: Thread<int> = thread.spawn(fn() -> int {
        let worker_res: Res = new Res("on-worker")
        worker_res.touch()
        return worker_res.n           // "on-worker" closes on the worker thread
    })
    let got: int = t.join()
    io.println("joined {got}")
}

fn cycles() {
    io.println("-- cycles --")
    var a: Ring = new Ring("a")
    var b: Ring = new Ring("b")
    a.next = some(b)
    b.next = some(a)                  // rc never hits zero: no deinit, ever
    io.println("ring linked")
}

fn defers() {
    io.println("-- defers --")
    let r: Res = new Res("deferred-owner")
    defer io.println("defer ran")     // defers first, then the frame releases
    io.println("body done")
}

fn main() {
    scopes_and_reassign()
    containers()
    inheritance()
    generics()
    supers()
    threads()
    cycles()
    defers()

    match Port.checked(0) {
        ok(p) => io.println("impossible {p.num}"),
        err(e) => io.println("refused: {e.msg}"),
    }
    match Port.checked(8080) {
        ok(p) => io.println("bound {p.num}"),   // p dies inside this arm
        err(e) => io.println("impossible {e.msg}"),
    }
    io.println("main ends")
}
