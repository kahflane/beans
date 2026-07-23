fn give_back(values: List<int>) -> List<int> {
    return values
}

fn consume(take values: List<int>) -> int {
    return values.len()
}

fn swap(inout left: int, inout right: int) {
    left = right
}

fn captures_inout(inout value: int) {
    let read: fn() -> int = fn() -> int { return value }
}

@move_only class Packet {
    id: int = 1
}

interface Edit {
    fn apply(self, inout value: int)
}

class WrongEdit : Edit {
    override fn apply(self, value: int) {}
}

fn main() {
    let values: List<int> = [1]
    let copied: List<int> = values
    let consumed: int = consume(values)

    let map: Map<string, int> = {"one": 1}
    let copied_map: Map<string, int> = map

    let nested: List<List<int>> = [[1], [2]]
    let nested_copy: List<List<int>> = nested.clone()

    let stored: fn(List<int>) -> int = consume

    let packet: Packet = {}
    let packet_copy: Packet = packet

    var left: int = 1
    let fixed: int = 2
    swap(left, inout fixed)
    swap(inout left, inout left)
    let escaped: int = inout left
}
