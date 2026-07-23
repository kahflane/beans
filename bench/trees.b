// binary-trees: build and drop many short-lived trees, checking each.
// The classic allocation+reclamation bench. beans is ARC with a pool
// allocator, so this measures the release cascade and the freelists, not
// just loop speed — the one dimension churn.b only touches shallowly.
import std.io
import std.os

class Node {
    left: Option<Node> = none
    right: Option<Node> = none
    value: int = 0
}

fn build(depth: int, seed: int) -> Node {
    var n: Node = Node { value: depth + seed }
    if depth > 0 {
        n.left = some(build(depth - 1, seed))
        n.right = some(build(depth - 1, seed))
    }
    return n
}

fn count(n: Node) -> int {
    var total: int = 1 + n.value
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
    let args: List<string> = os.args()
    let max_depth: int = if args.len() > 0 { args[0].to_int().or(16) } else { 16 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    let long_lived: Node = build(max_depth, seed) // stays alive to the very end
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
            total += count(build(d, seed))
            i += 1
        }
        d += 2
    }

    io.println("trees {total} long {count(long_lived)}")
}
