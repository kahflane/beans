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
    let n: int = if args.len() > 0 { args[0].to_int().or(40) } else { 40 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    io.println(fib(n) + seed - seed)
}
