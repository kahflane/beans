// Parallel compute with one spawned worker.
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
    let n: int = args.get(0).or("").to_int().or(1_000_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    let worker: Thread<int> = thread.spawn(fn() -> int { return mix(0, n, seed) })
    io.println("parallel_1 {worker.join()}")
}
