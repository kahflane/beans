// Native Option of a pointer value uses null for none and the value pointer
// itself for some. Keep every observable Option operation in the differential
// suite so that layout stays an implementation detail.
import std.io

fn pass(v: Option<string>) -> Option<string> {
    return v
}

fn main() {
    let a: Option<string> = some("bean")
    let b: Option<string> = some("BEAN".to_lower())
    let n: Option<string> = none

    io.println("niche {a} {n} {a.is_some()} {n.is_none()}")
    io.println("or {a.or("x")} {n.or("x")} {pass(b).expect("missing")}")

    match a {
        some(value) => { io.println("match {value}") }
        none => { io.println("bad") }
    }

    var values: List<string> = ["first"]
    io.println("list {values.get(0)} {values.get(1)}")
    io.println("pop {values.pop().or("fallback")} {values.pop().or("fallback")}")

    var counts: Map<Option<string>, int> = {}
    counts[a] = 7
    counts[n] = 9
    io.println("map {counts[b]} {counts[n]} {counts.len()}")
}
