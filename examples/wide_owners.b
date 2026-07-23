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
}

struct ArenaEdge {
    target: ArenaOwner
}

class ArenaOwner {
    inner: Arena<ArenaEdge>
}

fn boxed<T>(value: T) -> Box<T> {
    return Box.new(value)
}

fn store_one<T>(arena: Arena<T>, value: T) -> int {
    return arena.put(value)
}

fn make_cycles() {
    let seed_box: Box<Option<BoxOwner>> = Box.new(none)
    let box_owner: BoxOwner = BoxOwner { inner: take seed_box }
    box_owner.inner.set(some(box_owner))

    let seed_arena: Arena<ArenaEdge> = Arena.new(1)
    let arena_owner: ArenaOwner = ArenaOwner { inner: take seed_arena }
    arena_owner.inner.put(ArenaEdge { target: arena_owner })
}

fn main() {
    var event_box: Box<Event> = boxed(Event { label: "first", value: 10 })
    let first: Event = event_box.get()
    event_box.set(Event { label: "second", value: 20 })
    let second: Event = event_box.get()
    io.println("box {first.label} {first.value} {second.label} {second.value}")

    let array_box: Box<[i64; 2]> = Box.new([7, 8])
    let array: [i64; 2] = array_box.get()
    let decimal_box: Box<decimal> = Box.new(12.50)
    io.println("box values {array[0]} {array[1]} {decimal_box.get()}")

    var events: Arena<Event> = Arena.new(1)
    let one: int = store_one(events, Event { label: "one", value: 1 })
    let two: int = events.put(Event { label: "two", value: 2 })
    let read: Event = events.get(one).or(Event { label: "none", value: 0 })
    let direct: Event = events.at(two)
    io.println("arena {one} {two} {events.len()} {read.label} {direct.label}")
    events.clear()
    io.println("arena clear {events.len()} {events.get(one).or(Event { label: "missing", value: 0 }).label}")

    var decimals: Arena<decimal> = Arena.new(0)
    decimals.put(1.25)
    decimals.put(2.50)
    io.println("arena decimal {decimals.at(0)} {decimals.get(1).or(0.0)}")

    var results: Arena<Result<Pair>> = Arena.new(1)
    results.put(ok(Pair { left: 3, right: 4 }))
    results.put(err("arena error"))
    let result: Result<Pair> = results.at(1)
    match result {
        ok(value) => { io.println("bad {value.left}") },
        err(error) => { io.println("result {error.msg}") },
    }

    make_cycles()
}
