// Direct function calls over runtime data.
import std.io
import std.os

fn step(value: int, seed: int) -> int {
    let mixed: int = (value * 17 + seed) % 1_000_003
    if mixed % 3 == 0 {
        return mixed / 3
    }
    return mixed + 11
}

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(40_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var sum: int = 0
    var i: int = 0
    for i < n {
        sum += step(i + sum % 97, seed)
        i += 1
    }
    io.println("direct_calls {sum}")
}
