// A generic value chooser. Beans monomorphizes this for int.
import std.io
import std.os

fn choose<T>(left: T, right: T, use_left: bool) -> T {
    if use_left { return left }
    return right
}

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(400_000_000) } else { 400_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var sum: int = 0
    var i: int = 0
    for i < n {
        sum += choose((i + seed) % 101, (i + seed) % 67, i % 2 == 0)
        i += 1
    }
    io.println("generic_calls {sum}")
}
