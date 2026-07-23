import std.io

struct Pair {
    left: i32
    right: i32
}

struct Event {
    label: string
    value: int
}

enum Payload {
    pair(value: Pair)
    array(value: [i64; 2])
    event(value: Event)
    mixed(pair: Pair, label: string, values: [i64; 2])
    empty
}

enum Holder<T> {
    item(value: T)
    missing
}

fn describe(value: Payload) {
    match value {
        pair(pair) => { io.println("pair {pair.left} {pair.right}") },
        array(values) => { io.println("array {values[0]} {values[1]}") },
        event(event) => { io.println("event {event.label} {event.value}") },
        mixed(pair, label, values) => {
            io.println("mixed {pair.left} {label} {values[1]}")
        },
        empty => { io.println("empty") },
    }
}

fn describe_holder(value: Holder<Pair>) {
    match value {
        item(pair) => { io.println("holder {pair.left} {pair.right}") },
        missing => { io.println("holder missing") },
    }
}

fn main() {
    let pair: Payload = Payload.pair(Pair { left: 3, right: 4 })
    let array: Payload = Payload.array([5, 6])
    let event: Payload = Payload.event(Event { label: "launch", value: 7 })
    let mixed: Payload = Payload.mixed(
        Pair { left: 8, right: 9 }, "wide", [10, 11])
    let empty: Payload = Payload.empty

    describe(pair)
    describe(array)
    describe(event)
    describe(mixed)
    describe(empty)
    let holder: Holder<Pair> = Holder.item(Pair { left: 12, right: 13 })
    describe_holder(holder)

    var keyed: Map<Payload, int> = {}
    keyed[Payload.pair(Pair { left: 1, right: 2 })] = 12
    keyed[Payload.array([3, 4])] = 34
    keyed[Payload.event(Event { label: "same", value: 5 })] = 55
    io.println("keys {keyed[Payload.pair(Pair { left: 1, right: 2 })]} {keyed.contains(Payload.array([3, 4]))} {keyed.contains(Payload.event(Event { label: "same", value: 5 }))} {keyed.len()}")
}
