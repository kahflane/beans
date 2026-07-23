// List slice copies with runtime offsets and a deterministic reduction.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(8_000_000) } else { 8_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var values: List<int> = []
    values.reserve(n)
    var i: int = 0
    for i < n {
        values.push((i * 31 + seed) % 1_000_003)
        i += 1
    }
    let width: int = 1024
    let rounds: int = n / 20
    var checksum: int = 0
    i = 0
    for i < rounds {
        let start: int = (i * 97 + seed) % (n - width)
        let part: List<int> = values.slice(start, start + width)
        checksum += part[0] + part[width - 1] + part.len()
        i += 1
    }
    io.println("slices {checksum} {rounds}")
}
