// Calls through a plain closure and a captured closure. Both locals are
// immutable, so the native backend may devirtualize their code pointer.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(400_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    let plain: fn(int) -> int = fn(value: int) -> int { return value * 3 + 1 }
    let offset: int = seed % 97
    let captured: fn(int) -> int = fn(value: int) -> int { return value + offset }
    var sum: int = 0
    var i: int = 0
    for i < n {
        if i % 2 == 0 {
            sum += plain(i % 1009)
        } else {
            sum += captured(i % 1009)
        }
        i += 1
    }
    io.println("closures {sum}")
}
