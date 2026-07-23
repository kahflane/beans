// Typed bump storage: append values, read through stable handles, bulk reset.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(20_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var arena: Arena<int> = new Arena(4096)
    var state: int = seed + 1
    var sum: int = 0
    for i: int in 0..n {
        state = (state * 48271 + i + 1) % 2147483647
        let handle: int = arena.put(state)
        let pick: int = (state + i) % (handle + 1)
        state = (state + arena.at(pick)) % 2147483647
        sum += state
        if arena.len() == 4096 {
            arena.clear()
        }
    }
    io.println("sum {sum} left {arena.len()}")
}
