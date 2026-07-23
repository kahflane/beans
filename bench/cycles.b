// Strong two-node cycles. Beans reclaims these with trial deletion.
import std.io
import std.os

class Node {
    value: int
    next: Option<Node> = none

    fn init(value: int) { self.value = value }
}

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(2_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var checksum: int = 0
    var i: int = 0
    for i < n {
        let left: Node = new Node(i + seed)
        let right: Node = new Node(i + seed + 1)
        left.next = some(right)
        right.next = some(left)
        checksum += left.value + right.value
        i += 1
    }
    io.println("cycles {checksum}")
}
