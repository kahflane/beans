import std.io
import std.os

fn fib(n: int) -> int {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(40)
    let seed: int = args.get(1).or("").to_int().or(1)
    io.println(fib(n) + seed - seed)
}
