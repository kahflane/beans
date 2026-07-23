// allocation + refcount churn: the dimension fib/loops/shapes don't touch.
// 5M short-lived objects, each aliased once; 1 in 1000 survives into a list.
import std.io
import std.os

class P {
    a: int = 0
    b: int = 0

    fn init(a: int, b: int) {
        self.a = a
        self.b = b
    }
}

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(5_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var keep: List<P> = []
    keep.reserve(n / 1000 + 1)
    var sum: int = 0
    for i: int in 0..n {
        var p: P = new P(i + seed, i + seed + 1)
        var q: P = p
        sum += q.a + p.b
        if i % 1000 == 0 {
            keep.push(p)
        }
    }
    io.println("sum {sum} kept {keep.len()}")
}
