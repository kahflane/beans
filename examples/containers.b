// stdlib phase 3: the container extras — List first/last/min/insert/remove/
// index_of/reverse/clear/slice, stable sort + sort_by, Map remove/keys/
// values/clear, and Bytes value equality. Ends on the remove() panic, so the
// message and position must match the native binary byte for byte.
import std.io

fn main() {
    var xs: List<int> = [5, 1, 4, 1, 3]
    xs.reserve(32)
    io.println("{xs.first().or(-1)} {xs.last().or(-1)} {xs.min().or(-1)} {xs.max().or(-1)}")
    io.println("{xs.index_of(4).or(-1)} {xs.index_of(9).or(-1)}")
    xs.insert(0, 9)
    xs.insert(6, 7)
    io.println(xs)
    let gone: int = xs.remove(1)
    io.println("{gone} {xs}")
    xs.reverse()
    io.println(xs)
    let mid: List<int> = xs.slice(1, 4)
    io.println(mid)
    xs.sort()
    io.println(xs)

    var signed: List<int> = [70000, -100000, -1, 65536, 0, -99999]
    signed.sort()
    io.println(signed)

    // sort_by takes any less-than predicate; captures work
    let desc: bool = true
    xs.sort_by(fn(a: int, b: int) -> bool {
        if desc {
            return a > b
        }
        return a < b
    })
    io.println(xs)
    xs.clear()
    io.println("{xs.len()}")

    var empty: List<int> = []
    io.println("{empty.first().or(-1)} {empty.min().or(-1)} {empty} {empty.slice(0, 0)}")

    var names: List<string> = ["pear", "fig", "apple", "kiwi"]
    names.sort()
    io.println(names.join(" "))
    names.sort_by(fn(a: string, b: string) -> bool { return a.len() < b.len() })
    io.println(names.join(" "))

    // sort_by_key evaluates the key once per element and remains stable.
    var keyed: List<string> = ["bbb", "a", "ccc", "d", "ee"]
    keyed.sort_by_key(fn(value: string) -> int { return value.len() })
    io.println(keyed.join(" "))

    names.insert(1, "plum")
    let popped: string = names.remove(4)
    io.println("{popped} | {names} | {names.slice(1, 3)}")

    // sorts are stable: 1.50 and 1.5 compare equal, insertion order survives
    var ds: List<decimal> = [1.50, 1.5, 0.75]
    ds.sort()
    io.println(ds)

    var m: Map<string, int> = {}
    m.reserve(16)
    m["one"] = 1
    m["two"] = 2
    m["three"] = 3
    io.println("{m.remove("two")} {m.remove("nope")} {m.len()}")
    match m.get("one") {
        some(value) => { io.println("map match {value}") },
        none => { io.println("map match missing") },
    }
    match m.get("gone") {
        some(value) => { io.println("bad map match {value}") },
        none => { io.println("map match none") },
    }
    io.println(m.keys())
    io.println(m.values())
    m.clear()
    io.println("{m.len()} {m.contains("one")}")

    var byid: Map<int, string> = {}
    byid[7] = "seven"
    byid[9] = "nine"
    io.println("{byid.remove(7)} {byid.keys()} {byid.values()}")

    // Bytes compares by value: length + contents
    let a: Bytes = Bytes.new(4).put_u16(0, 500)
    let b: Bytes = Bytes.new(4).put_u16(0, 500)
    let c: Bytes = Bytes.new(3)
    io.println("{a == b} {a != b} {a == c}")

    let boom: int = mid.remove(9)
    io.println("{boom}")
}
