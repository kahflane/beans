// Unique heap ownership: every Box is created, read, changed, and dropped.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(10_000_000) } else { 10_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var sum: int = 0
    var batch: List<Box<int>> = []
    batch.reserve(1024)
    for i: int in 0..n {
        let value: Box<int> = Box.new(i + seed)
        sum += value.get()
        value.set(value.get() + 3)
        sum += value.get()
        batch.push(take value)
        if batch.len() == 1024 {
            batch.clear()
        }
    }
    io.println("sum {sum}")
}
