// Sequential thread spawn/join with captured runtime values.
import std.io
import std.os
import std.thread

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(10_000) } else { 10_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var checksum: int = 0
    var i: int = 0
    for i < n {
        let value: int = i
        let worker: Thread<int> = thread.spawn(fn() -> int {
            return (value * 17 + seed) % 1_000_003
        })
        checksum += worker.join()
        i += 1
    }
    io.println("thread_spawn {checksum}")
}
