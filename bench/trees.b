// binary-trees: build and drop many short-lived trees, checking each.
// The classic allocation+reclamation bench. beans is ARC with a pool
// allocator, so this measures the release cascade and the freelists, not
// just loop speed — the one dimension churn.b only touches shallowly.
import std.io

class Node {
    left: Option<Node> = none
    right: Option<Node> = none
}

fn build(depth: int) -> Node {
    var n: Node = Node {}
    if depth > 0 {
        n.left = some(build(depth - 1))
        n.right = some(build(depth - 1))
    }
    return n
}

fn count(n: Node) -> int {
    var total: int = 1
    match n.left {
        some(l) => { total += count(l) }
        none => { }
    }
    match n.right {
        some(r) => { total += count(r) }
        none => { }
    }
    return total
}

fn main() {
    let max_depth: int = 14
    let long_lived: Node = build(max_depth) // stays alive to the very end
    var total: int = 0

    var d: int = 4
    for d <= max_depth {
        // iters = 2^(max_depth - d + 4); no shift operator, so multiply
        var iters: int = 1
        var k: int = max_depth - d + 4
        for k > 0 {
            iters *= 2
            k -= 1
        }
        var i: int = 0
        for i < iters {
            total += count(build(d))
            i += 1
        }
        d += 2
    }

    io.println("trees {total} long {count(long_lived)}")
}
