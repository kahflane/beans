// Synthetic graph traversal with a List work stack and Map visited set.
import std.io
import std.os

fn main() {
    let args: List<string> = os.args()
    let n: int = args.get(0).or("").to_int().or(2_000_000)
    let seed: int = args.get(1).or("").to_int().or(1)
    var work: List<int> = [0]
    var seen: Map<int, bool> = {}
    work.reserve(n)
    seen.reserve(n)
    var checksum: int = 0
    var edges: int = 0
    for work.len() > 0 {
        let node: int = work.pop().or(-1)
        if seen.insert(node, true) {
            checksum += node
            if node + 1 < n {
                work.push(node + 1)
                edges += 1
            }
            work.push((node * 17 + seed) % n)
            work.push((node * 31 + seed + 7) % n)
            edges += 2
        }
    }
    io.println("graph {seen.len()} {edges} {checksum}")
}
