// UTF-8 character walking. String len remains byte-based.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(100_000) } else { 100_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    let text: string = "héllo→🌍".repeat(n + seed % 3)
    let rounds: int = 50
    var count: int = 0
    var bytes: int = 0
    var round: int = 0
    for round < rounds {
        let limit: int = text.len() - (round % 3) * 13
        count += text.count_chars(0, limit)
        bytes += limit
        round += 1
    }
    io.println("utf8 {count} {bytes} {text.len()}")
}
