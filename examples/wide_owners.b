import std.io

struct Pair {
    left: i32
    right: i32
}

struct Event {
    label: string
    value: int
}

class BoxOwner {
    inner: Box<Option<BoxOwner>>

    fn init(move inner: Box<Option<BoxOwner>>) { self.inner = move inner }
}

struct ArenaEdge {
    target: ArenaOwner
}

class ArenaOwner {
    inner: Arena<ArenaEdge>

    fn init(move inner: Arena<ArenaEdge>) { self.inner = move inner }
}

fn boxed<T>(value: T) -> Box<T> {
    return new Box(value)
}

fn store_one<T>(arena: Arena<T>, value: T) -> int {
    return arena.put(value)
}

fn make_cycles() {
    let seed_box: Box<Option<BoxOwner>> = new Box(none)
    let box_owner: BoxOwner = new BoxOwner(move seed_box)
    box_owner.inner.set(some(box_owner))

    let seed_arena: Arena<ArenaEdge> = new Arena(1)
    let arena_owner: ArenaOwner = new ArenaOwner(move seed_arena)
    arena_owner.inner.put(ArenaEdge { target: arena_owner })
}

fn main() {
    var event_box: Box<Event> = boxed(Event { label: "first", value: 10 })
    let first: Event = event_box.get()
    event_box.set(Event { label: "second", value: 20 })
    let second: Event = event_box.get()
    io.println("box {first.label} {first.value} {second.label} {second.value}")

    let array_box: Box<[i64; 2]> = new Box([7, 8])
    let array: [i64; 2] = array_box.get()
    let decimal_box: Box<decimal> = new Box(12.50)
    io.println("box values {array[0]} {array[1]} {decimal_box.get()}")

    var events: Arena<Event> = new Arena(1)
    let one: int = store_one(events, Event { label: "one", value: 1 })
    let two: int = events.put(Event { label: "two", value: 2 })
    let read: Event = events.get(one).or(Event { label: "none", value: 0 })
    let direct: Event = events.at(two)
    io.println("arena {one} {two} {events.len()} {read.label} {direct.label}")
    events.clear()
    io.println("arena clear {events.len()} {events.get(one).or(Event { label: "missing", value: 0 }).label}")

    var decimals: Arena<decimal> = new Arena(0)
    decimals.put(1.25)
    decimals.put(2.50)
    io.println("arena decimal {decimals.at(0)} {decimals.get(1).or(0.0)}")

    var results: Arena<Result<Pair>> = new Arena(1)
    results.put(ok(Pair { left: 3, right: 4 }))
    results.put(err("arena error"))
    let result: Result<Pair> = results.at(1)
    match result {
        ok(value) => { io.println("bad {value.left}") },
        err(error) => { io.println("result {error.msg}") },
    }

    make_cycles()
}
