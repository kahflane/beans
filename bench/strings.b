// string building and scanning: interpolation, join, split, upper, search,
// replace. Phase 1 added the string method set and O(1) length; this is the
// bench that exercises them under load.
import std.io

fn main() {
    let n: int = 800000

    var parts: List<string> = []
    var i: int = 0
    for i < n {
        parts.push("item{i}")
        i += 1
    }

    let joined: string = parts.join(",")
    let up: string = joined.to_upper()
    let back: List<string> = joined.split(",")

    var hits: int = 0
    for p: string in back {
        if p.contains("999") {
            hits += 1
        }
    }

    let swapped: string = joined.replace("item", "row")

    io.println("strings {joined.len()} {up.len()} {back.len()} {hits} {swapped.len()}")
}
