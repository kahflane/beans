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
    let n: int = if args.len() > 0 { args[0].to_int().or(40_000_000) } else { 40_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var sum: int = 0
    var i: int = 0
    for i < n {
        sum += step(i + sum % 97, seed)
        i += 1
    }
    io.println("direct_calls {sum}")
}
