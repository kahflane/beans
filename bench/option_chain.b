// Option-heavy acyclic linked structure.
import std.io
import std.os

class Node {
    value: int
    next: Option<Node> = none
}

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(1_000_000) } else { 1_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var head: Option<Node> = none
    var i: int = 0
    for i < n {
        let node: Node = Node { value: i + seed, next: head }
        head = some(node)
        i += 1
    }
    var cursor: Option<Node> = head
    var checksum: int = 0
    i = 0
    for i < n {
        match cursor {
            some(node) => {
                checksum += node.value
                cursor = node.next
            },
            none => {},
        }
        i += 1
    }
    io.println("option_chain {checksum} {head.is_some()}")
}
