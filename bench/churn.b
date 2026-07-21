// allocation + refcount churn: the dimension fib/loops/shapes don't touch.
// 5M short-lived objects, each aliased once; 1 in 1000 survives into a list.
import std.io

class P {
    a: int = 0
    b: int = 0
}

fn main() {
    var keep: List<P> = []
    var sum: int = 0
    for i: int in 0..5000000 {
        var p: P = P { a: i, b: i + 1 }
        var q: P = p
        sum += q.a + p.b
        if i % 1000 == 0 {
            keep.push(p)
        }
    }
    io.println("sum {sum} kept {keep.len()}")
}
