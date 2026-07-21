// reference cycles — plain RC leaks these; the cycle collector frees them.
// run vs build must print the same; the native binary must stay flat on
// memory and report 0 leaked bytes.
import std.io

class Node {
    id: int = 0
    next: Option<Node> = none
}

fn ring(n: int) -> Node {
    var first: Node = Node { id: 0 }
    var prev: Node = first
    for i: int in 1..n {
        var cur: Node = Node { id: i }
        prev.next = some(cur)
        prev = cur
    }
    prev.next = some(first) // close the ring
    return first
}

fn churn(iters: int) {
    for i: int in 0..iters {
        var a: Node = Node { id: i }
        var b: Node = Node { id: i + 1 }
        a.next = some(b)
        b.next = some(a) // pair cycle, dropped every iteration
    }
}

fn main() {
    churn(200000)

    // a live ring must survive the collections the churn triggers
    let keep: Node = ring(1000)
    churn(200000)
    match keep.next {
        some(n) => io.println("ring alive, second id {n.id}"),
        none => io.println("broken"),
    }

    // self cycle
    var s: Node = Node { id: 99 }
    s.next = some(s)
    io.println("self {s.id}")

    // a big dropped ring exercises the iterative walk — must not smash the C stack
    var big: Node = ring(300000)
    big = Node { id: -1 }
    churn(1000)
    io.println("big dropped, now {big.id}")

    // closure capturing its own cell — a closure<->cell cycle
    var g: fn(int) -> int = fn(x: int) -> int { return x }
    g = fn(x: int) -> int {
        if x <= 0 { return 0 }
        return x + g(x - 1)
    }
    io.println("g {g(4)}")

    io.println("done")
}
