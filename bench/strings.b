// string building and scanning: interpolation, join, split, upper, search,
// replace. Phase 1 added the string method set and O(1) length; this is the
// bench that exercises them under load.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(2_000_000) } else { 2_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }

    var parts: List<string> = []
    parts.reserve(n)
    var i: int = 0
    for i < n {
        parts.push("item{seed}_{i}")
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
