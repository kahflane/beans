// hash map throughput: insert, read back, hit/miss probes, and the same
// again with string keys. Phase 0 made map reads native and nothing ever
// timed them; this is that number.
//
// Map carries an open-addressed hash index beside its insertion-ordered
// entry array, so get/set/contains are O(1) — these sizes are a real
// workload, comparable across the three languages.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(400000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var m: Map<int, int> = {}
    m.reserve(n)
    var i: int = 0
    for i < n {
        m[i + seed] = i * 2 + seed
        i += 1
    }

    var sum: int = 0
    i = 0
    for i < n {
        sum += m[i + seed]
        i += 1
    }

    // half of these hit, half miss
    var hits: int = 0
    i = 0
    for i < n {
        if m.contains(i + seed) { hits += 1 }
        if m.contains(i + n + seed) { hits += 1 }
        i += 1
    }

    // string keys go through hashing and comparison, not just an int mix
    let sn: int = n / 5
    var sm: Map<string, int> = {}
    sm.reserve(sn)
    i = 0
    for i < sn {
        sm["key{seed}_{i}"] = i + seed
        i += 1
    }
    var ssum: int = 0
    i = 0
    for i < sn {
        ssum += sm["key{seed}_{i}"]
        i += 1
    }

    io.println("maps {sum} {hits} {ssum} {m.len()} {sm.len()}")
}
