// List growth, iteration, slicing, and stable primitive sorting.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(5_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var values: List<int> = []
    values.reserve(n)
    var i: int = 0
    for i < n {
        values.push((i * 48271 + seed) % 1000003)
        i += 1
    }
    values.sort()
    var sum: int = 0
    for value: int in values {
        sum += value
    }
    let start: int = n / 4
    let stop: int = if start + 1000 < n { start + 1000 } else { n }
    let middle: List<int> = values.slice(start, stop)
    var middle_sum: int = 0
    for value: int in middle {
        middle_sum += value
    }
    io.println("sequences {sum} {middle_sum} {values.first().or(-1)} {values.last().or(-1)} {values.len()}")
}
