// Mixed inline struct, string, List, Map, allocation, and sorting work.
import std.io
import std.os

struct Event {
    user: int
    label: string
    value: int
}

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(1_500_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var events: List<Event> = []
    events.reserve(n)
    var i: int = 0
    for i < n {
        let user: int = (i * 17 + seed) % 8192
        events.push(Event { user: user, label: "event{(i + seed) % 32}", value: (i * 31 + seed) % 1000 })
        i += 1
    }
    var totals: Map<int, int> = {}
    totals.reserve(8192)
    var selected: List<int> = []
    selected.reserve(n / 7 + 1)
    for event: Event in events {
        totals[event.user] = totals.get(event.user).or(0) + event.value + event.label.len()
        if (event.user + seed) % 7 == 0 { selected.push(event.value) }
    }
    selected.sort()
    var checksum: int = 0
    i = 0
    for i < 8192 {
        checksum += totals.get(i).or(0)
        i += 1
    }
    let middle: int = selected[selected.len() / 2]
    io.println("mixed_app {checksum} {totals.len()} {selected.len()} {middle}")
}
