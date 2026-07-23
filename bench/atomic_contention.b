// Four workers contend on one atomic counter.
import std.io
import std.os
import std.thread

fn add_many(counter: AtomicInt, n: int) -> int {
    var i: int = 0
    for i < n {
        counter.add(1)
        i += 1
    }
    return n
}

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(10_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    let counter: AtomicInt = new AtomicInt(seed)
    let q: int = n / 4
    let t0: Thread<int> = thread.spawn(fn() -> int { return add_many(counter, q) })
    let t1: Thread<int> = thread.spawn(fn() -> int { return add_many(counter, q) })
    let t2: Thread<int> = thread.spawn(fn() -> int { return add_many(counter, q) })
    let t3: Thread<int> = thread.spawn(fn() -> int { return add_many(counter, n - q * 3) })
    let done: int = t0.join() + t1.join() + t2.join() + t3.join()
    io.println("atomic_contention {counter.get()} {done}")
}
