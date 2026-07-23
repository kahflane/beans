// Four independent workers: spawn/join overhead plus parallel integer work.
import std.io
import std.os
import std.thread

fn mix(start: int, stop: int, seed: int) -> int {
    var sum: int = 0
    var i: int = start
    for i < stop {
        sum += (i + seed) % 7
        i += 1
    }
    return sum
}

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(1_000_000_000) } else { 1_000_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    let q: int = n / 4
    let t0: Thread<int> = thread.spawn(fn() -> int { return mix(0, q, seed) })
    let t1: Thread<int> = thread.spawn(fn() -> int { return mix(q, q * 2, seed) })
    let t2: Thread<int> = thread.spawn(fn() -> int { return mix(q * 2, q * 3, seed) })
    let t3: Thread<int> = thread.spawn(fn() -> int { return mix(q * 3, n, seed) })
    let sum: int = t0.join() + t1.join() + t2.join() + t3.join()
    io.println("parallel {sum}")
}
