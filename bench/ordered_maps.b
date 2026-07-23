// Insertion-ordered hash map with remove/reinsert order changes.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(2_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var values: OrderedMap<int, int> = {}
    values.reserve(n)
    var i: int = 0
    for i < n {
        let key: int = (i * 48271 + seed) % n
        values[key] = i + seed
        i += 1
    }
    i = 0
    for i < n {
        if i % 5 == 0 {
            let key: int = (i * 48271 + seed) % n
            values.remove(key)
        }
        i += 1
    }
    i = n - 1
    for i >= 0 {
        if i % 5 == 0 {
            let key: int = (i * 48271 + seed) % n
            values[key] = i + seed + 1
        }
        i -= 1
    }
    let keys: List<int> = values.keys()
    var checksum: int = 0
    i = 0
    for i < keys.len() {
        checksum += keys[i] * ((i % 97) + 1)
        i += 1
    }
    io.println("ordered_maps {checksum} {values.len()} {keys[0]} {keys[keys.len() - 1]}")
}
