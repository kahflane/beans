// Bounded random insert/remove churn on a contiguous List.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(1_000_000) } else { 1_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var values: List<int> = []
    values.reserve(4096)
    var i: int = 0
    for i < 2048 {
        values.push(i + seed)
        i += 1
    }
    var checksum: int = 0
    i = 0
    for i < n {
        let at: int = (i * 48271 + seed) % values.len()
        checksum += values.remove(at)
        values.insert((at * 17 + seed) % (values.len() + 1), i + seed)
        i += 1
    }
    io.println("sequence_churn {checksum} {values.len()} {values[0]}")
}
