// Four workers protect an ARC class inside Mutex<T>.
import std.io
import std.os
import std.thread

class Counter {
    value: int = 0
}

fn bump_many(counter: Mutex<Counter>, n: int) -> int {
    var i: int = 0
    for i < n {
        counter.with(fn(value: Counter) { value.value += 1 })
        i += 1
    }
    return n
}

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(5_000_000) } else { 5_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    let counter: Mutex<Counter> = Mutex.new(Counter { value: seed })
    let q: int = n / 4
    let t0: Thread<int> = thread.spawn(fn() -> int { return bump_many(counter, q) })
    let t1: Thread<int> = thread.spawn(fn() -> int { return bump_many(counter, q) })
    let t2: Thread<int> = thread.spawn(fn() -> int { return bump_many(counter, q) })
    let t3: Thread<int> = thread.spawn(fn() -> int { return bump_many(counter, n - q * 3) })
    let done: int = t0.join() + t1.join() + t2.join() + t3.join()
    var final: int = 0
    counter.with(fn(value: Counter) { final = value.value })
    io.println("mutex_contention {final} {done}")
}
