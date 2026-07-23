// A deep unique chain must drop iteratively instead of using the C stack.
import std.io
import std.os

class Node {
    value: int
    next: Option<Node> = none
}

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(4_000_000) } else { 4_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var head: Node = Node { value: seed }
    var i: int = 1
    for i < n {
        let next: Node = Node { value: i + seed, next: some(head) }
        head = next
        i += 1
    }
    let last: int = head.value
    head = Node { value: -1 }
    io.println("deep_teardown {last} {head.value}")
}
