import std.io

fn main() {
    var sum: int = 0
    for i: int in 1..=200_000_000 {
        sum += i % 7
    }
    io.println(sum)
}
