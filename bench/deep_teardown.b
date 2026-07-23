// A deep unique chain must drop iteratively instead of using the C stack.
import std.io
import std.os

class Node {
    value: int
    next: Option<Node> = none

    fn init(value: int, next: Option<Node>) {
        self.value = value
        self.next = next
    }
}

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(4_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var head: Node = new Node(seed, none)
    var i: int = 1
    for i < n {
        let next: Node = new Node(i + seed, some(head))
        head = next
        i += 1
    }
    let last: int = head.value
    head = new Node(-1, none)
    io.println("deep_teardown {last} {head.value}")
}
