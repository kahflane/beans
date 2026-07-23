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
    let n: int = if args.len() > 0 { args[0].to_int().or(10_000_000) } else { 10_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    let counter: AtomicInt = AtomicInt.new(seed)
    let q: int = n / 4
    let t0: Thread<int> = thread.spawn(fn() -> int { return add_many(counter, q) })
    let t1: Thread<int> = thread.spawn(fn() -> int { return add_many(counter, q) })
    let t2: Thread<int> = thread.spawn(fn() -> int { return add_many(counter, q) })
    let t3: Thread<int> = thread.spawn(fn() -> int { return add_many(counter, n - q * 3) })
    let done: int = t0.join() + t1.join() + t2.join() + t3.join()
    io.println("atomic_contention {counter.get()} {done}")
}
