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

class Holder {
    event: Event
}

fn singleton<T>(value: T) -> List<T> {
    return [value]
}

fn label(number: int) -> string {
    return "event{number}"
}

fn main() {
    var pairs: List<Pair> = [
        Pair { left: 1, right: 2 },
        Pair { left: 3, right: 4 },
    ]
    pairs.push(Pair { left: 5, right: 6 })
    pairs.reserve(20)
    pairs.insert(1, Pair { left: 7, right: 8 })
    pairs[0] = Pair { left: 9, right: 10 }
    let removed: Pair = pairs.remove(2)
    let read: Pair = pairs.get(1).or(Pair { left: -1, right: -1 })
    let first: Pair = pairs.first().or(Pair { left: -2, right: -2 })
    let last: Pair = pairs.last().or(Pair { left: -3, right: -3 })
    var sum: i32 = 0
    for pair: Pair in pairs {
        sum += pair.left + pair.right
    }
    let copy: List<Pair> = pairs.clone()
    let middle: List<Pair> = copy.slice(1, 3)
    pairs.reverse()
    let popped: Pair = pairs.pop().or(Pair { left: -4, right: -4 })
    let generic: List<Pair> = singleton(Pair { left: 31, right: 32 })
    pairs.clear()
    io.println("pair {removed.left} {removed.right} {read.left} {first.right} {last.left} {sum} {middle[0].right} {popped.left} {pairs.len()} {generic[0].right}")

    var arrays: List<[i64; 2]> = [[11, 12], [13, 14]]
    arrays.push([15, 16])
    let gone: [i64; 2] = arrays.remove(1)
    io.println("array {arrays[1][0]} {gone[1]} {arrays.pop().or([0, 0])[1]}")

    var results: List<Result<Pair>> = [
        ok(Pair { left: 21, right: 22 }),
        err("wide list error"),
    ]
    let results_copy: List<Result<Pair>> = results.clone()
    let error_copy: Result<Pair> = results_copy.get(1).or(ok(Pair { left: 0, right: 0 }))
    match error_copy {
        ok(value) => { io.println("bad result {value.left}") }
        err(error) => { io.println("result {error.msg}") }
    }
    results.clear()
    let good_result: Result<Pair> = results_copy.remove(0)
    match good_result {
        ok(value) => { io.println("result pair {value.left} {value.right}") }
        err(error) => { io.println("bad error {error.msg}") }
    }

    var event: Event = Event { user: 7, label: label(1), value: 40 }
    let event_copy: Event = event
    event.label = label(2)
    var events: List<Event> = [event_copy]
    events.push(event)
    let holder: Holder = Holder { event: events[0] }
    let cloned_events: List<Event> = events.clone()
    events[0] = Event { user: 8, label: label(3), value: 50 }
    events.clear()
    let held: Event = cloned_events.get(0).or(Event { user: 0, label: label(0), value: 0 })
    let moved: Event = cloned_events.remove(1)
    io.println("owned struct {event_copy.label} {event.label} {holder.event.label} {held.label} {moved.label} {cloned_events.len()}")

    var fixed_events: [Event; 2] = [event_copy, event]
    let fixed_copy: [Event; 2] = fixed_events
    fixed_events[0] = Event { user: 9, label: label(4), value: 60 }
    io.println("owned array {fixed_copy[0].label} {fixed_events[0].label} {fixed_copy[1].label}")

    var decimals: List<decimal> = [1.50, 1.5, 0.75, 2.25]
    let decimal_copy: List<decimal> = decimals.clone()
    decimals.sort_by(fn(a: decimal, b: decimal) -> bool { return a > b })
    decimals.sort_by_key(fn(value: decimal) -> int { return value as int })
    decimals.sort()
    io.println("decimal {decimals} {decimals.min().or(0.0)} {decimals.max().or(0.0)} {decimals.contains(1.5)} {decimals.index_of(2.25).or(-1)} {decimal_copy.join("|")}")
}
