import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(200_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var sum: int = 0
    for i: int in 1..=n {
        sum += (i + seed) % 7
    }
    io.println(sum)
}
