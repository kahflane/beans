// In-memory form of the append-only KV path: write, restart scan, lookup,
// and compaction. Disk/page-cache timing belongs in the separate systems run.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(3_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    let key_count: int = n / 4 + 1
    let log: Bytes = new Bytes(0)
    log.reserve(n * 24)
    var latest: Map<int, int> = {}
    latest.reserve(key_count)
    var i: int = 0
    for i < n {
        let key: int = (i * 48271 + seed) % key_count
        let value: int = i * 17 + seed
        latest[key] = log.len()
        log.append_i64(key).append_i64(value).append_i64(i)
        i += 1
    }

    var rebuilt: Map<int, int> = {}
    rebuilt.reserve(key_count)
    var pos: int = 0
    for pos < log.len() {
        rebuilt[log.get_i64(pos)] = pos
        pos += 24
    }

    let compact: Bytes = new Bytes(0)
    compact.reserve(key_count * 24)
    var checksum: int = 0
    i = 0
    for i < key_count {
        match rebuilt.get(i) {
            some(at) => {
                checksum += log.get_i64(at + 8)
                compact.append_range(log, at, at + 24)
            },
            none => {},
        }
        i += 1
    }
    io.println("kv_store {checksum} {latest.len()} {rebuilt.len()} {log.len()} {compact.len()}")
}
