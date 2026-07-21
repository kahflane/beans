// a long acyclic chain dropped at once — the release cascade must be
// iterative in both backends, or 400k nodes smash the stack. The cycle
// collector covers rings (examples/cycles.b); this covers straight chains.
import std.io

class Node {
    next: Option<Node> = none
    id: int = 0
}

fn main() {
    var head: Node = Node { id: 0 }
    for i: int in 1..400000 {
        var n: Node = Node { id: i }
        n.next = some(head)
        head = n
    }
    head = Node { id: 0 - 1 }
    io.println("alive {head.id}")
}
