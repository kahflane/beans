// Repeated map updates, removals, misses, and reinsertion.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(10_000_000) } else { 10_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    let keys: int = n / 4 + 1
    var values: Map<int, int> = {}
    values.reserve(keys)
    var i: int = 0
    for i < n {
        let key: int = (i * 17 + seed) % keys
        values[key] = i + seed
        if i % 5 == 0 { values.remove((key + 3) % keys) }
        i += 1
    }
    var checksum: int = 0
    var hits: int = 0
    i = 0
    for i < keys {
        match values.get(i) {
            some(value) => {
                checksum += value
                hits += 1
            },
            none => {},
        }
        i += 1
    }
    io.println("map_churn {checksum} {hits} {values.len()}")
}
