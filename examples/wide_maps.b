import std.io

struct Pair {
    left: i32
    right: i32
}

struct Event {
    user: int
    label: string
    value: int
}

struct Edge {
    target: Node
}

class Node {
    edges: Map<int, Edge>

    fn init(move edges: Map<int, Edge>) { self.edges = move edges }
}

struct KeyEdge {
    target: KeyNode
}

class KeyNode {
    edges: Map<KeyEdge, int>

    fn init(move edges: Map<KeyEdge, int>) { self.edges = move edges }
}

fn event(number: int, value: int) -> Event {
    return Event { user: number, label: "event{number}", value: value }
}

fn make_typed_map_cycle() {
    let node: Node = new Node({})
    node.edges[1] = Edge { target: node }
}

fn make_typed_key_cycle() {
    let node: KeyNode = new KeyNode({})
    node.edges[KeyEdge { target: node }] = 1
}

fn main() {
    var events: Map<int, Event> = {}
    events.reserve(24)
    for i: int in 0..12 {
        events[i] = event(i, i * 10)
    }
    events.set(3, event(30, 303))
    let duplicate: bool = events.insert(3, event(31, 313))
    let inserted: bool = events.insert(12, event(12, 120))
    let found: Event = events.get(3).or(event(-1, -1))
    let missing: Event = events.get(99).or(event(99, 999))
    let direct: Event = events[4]
    let copy: Map<int, Event> = events.clone()
    let values: List<Event> = copy.values()
    var total: int = 0
    for value: Event in values {
        total += value.value
    }
    io.println("events {duplicate} {inserted} {found.label} {found.value} {missing.label} {direct.label} {events.len()} {copy.len()} {values.len()} {total}")
    io.println("remove {events.remove(5)} {events.remove(5)} {events.len()} {copy[5].label}")
    events.clear()
    io.println("clear {events.len()} {copy.len()} {copy.contains(3)}")

    var ordered: OrderedMap<int, [i64; 2]> = {}
    for i: int in 0..12 {
        ordered[i] = [i, i + 100]
    }
    for i: int in 0..7 {
        ordered.remove(i)
    }
    ordered[20] = [20, 120]
    let ordered_values: List<[i64; 2]> = ordered.values()
    io.println("ordered {ordered.keys()} {ordered_values[0][0]} {ordered_values[5][1]} {ordered.len()}")

    var ledger: Map<string, decimal> = {}
    ledger["a"] = 1.25
    ledger["b"] = 2.50
    ledger.set("a", 3.75)
    let ledger_copy: Map<string, decimal> = ledger.clone()
    io.println("decimal {ledger["a"]} {ledger.get("b").or(0.0)} {ledger.values()} {ledger_copy.len()}")

    var results: Map<int, Result<Pair>> = {}
    results[1] = ok(Pair { left: 7, right: 8 })
    results[2] = err("map error")
    let result_copy: Map<int, Result<Pair>> = results.clone()
    let taken: Result<Pair> = result_copy[2]
    match taken {
        ok(value) => { io.println("bad result {value.left}") },
        err(error) => { io.println("result {error.msg} {result_copy.values().len()}") },
    }
    results.clear()

    var pairs: Map<Pair, Event> = {}
    pairs[Pair { left: 1, right: 2 }] = event(1, 12)
    pairs.set(Pair { left: 3, right: 4 }, event(3, 34))
    let pair_duplicate: bool = pairs.insert(
        Pair { left: 1, right: 2 }, event(9, 99))
    let pair_copy: Map<Pair, Event> = pairs.clone()
    let pair_keys: List<Pair> = pair_copy.keys()
    io.println("wide keys {pair_duplicate} {pairs[Pair { left: 1, right: 2 }].label} {pairs.contains(Pair { left: 3, right: 4 })} {pairs.remove(Pair { left: 3, right: 4 })} {pairs.len()} {pair_copy.len()} {pair_keys.len()}")

    var arrays: OrderedMap<[i64; 2], int> = {}
    arrays[[1, 2]] = 12
    arrays[[3, 4]] = 34
    let array_first: int = arrays[[1, 2]]
    let array_second: int = arrays.get([3, 4]).or(-1)
    io.println("array keys {array_first} {array_second} {arrays.keys().len()}")

    var optional: Map<Option<Pair>, int> = {}
    optional[some(Pair { left: 5, right: 6 })] = 56
    optional[none] = 0
    let option_some: int = optional[some(Pair { left: 5, right: 6 })]
    let option_none: int = optional[none]
    io.println("option keys {option_some} {option_none} {optional.len()}")

    var events_by_event: Map<Event, int> = {}
    events_by_event[event(7, 70)] = 1
    let same_event: bool = events_by_event.contains(event(7, 70))
    let arc_label: string = events_by_event.keys()[0].label
    io.println("arc key {same_event} {arc_label}")

    // The cycle walker must see ARC pointers nested inside wide map values.
    make_typed_map_cycle()
    // It must also see ARC pointers nested inside compiler-boxed wide keys.
    make_typed_key_cycle()
}
