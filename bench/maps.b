// hash map throughput: insert, read back, hit/miss probes, and the same
// again with string keys. Phase 0 made map reads native and nothing ever
// timed them; this is that number.
//
// Map carries an open-addressed hash index beside its insertion-ordered
// entry array, so get/set/contains are O(1) — these sizes are a real
// workload, comparable across the three languages.
import std.io

fn main() {
    let n: int = 400000
    var m: Map<int, int> = {}
    var i: int = 0
    for i < n {
        m[i] = i * 2
        i += 1
    }

    var sum: int = 0
    i = 0
    for i < n {
        sum += m[i]
        i += 1
    }

    // half of these hit, half miss
    var hits: int = 0
    i = 0
    for i < n {
        if m.contains(i) { hits += 1 }
        if m.contains(i + n) { hits += 1 }
        i += 1
    }

    // string keys go through hashing and comparison, not just an int mix
    let sn: int = 80000
    var sm: Map<string, int> = {}
    i = 0
    for i < sn {
        sm["key{i}"] = i
        i += 1
    }
    var ssum: int = 0
    i = 0
    for i < sn {
        ssum += sm["key{i}"]
        i += 1
    }

    io.println("maps {sum} {hits} {ssum} {m.len()} {sm.len()}")
}
