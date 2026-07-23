import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(200_000_000) } else { 200_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var sum: int = 0
    for i: int in 1..=n {
        sum += (i + seed) % 7
    }
    io.println(sum)
}
