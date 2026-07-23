// Strong two-node cycles. Beans reclaims these with trial deletion.
import std.io
import std.os

class Node {
    value: int
    next: Option<Node> = none
}

fn main() {
    let args: List<string> = os.args()
    let n: int = if args.len() > 0 { args[0].to_int().or(2_000_000) } else { 2_000_000 }
    let seed: int = if args.len() > 1 { args[1].to_int().or(1) } else { 1 }
    var checksum: int = 0
    var i: int = 0
    for i < n {
        let left: Node = Node { value: i + seed }
        let right: Node = Node { value: i + seed + 1 }
        left.next = some(right)
        right.next = some(left)
        checksum += left.value + right.value
        i += 1
    }
    io.println("cycles {checksum}")
}
