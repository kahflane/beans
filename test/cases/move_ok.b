import std.io

class Item {
    value: int = 0
}

fn make(value: int) -> Item {
    let item: Item = Item { value: value }
    return take item
}

fn read(item: Item) -> int {
    return item.value
}

fn round_trip(take values: List<int>) -> List<int> {
    values.push(5)
    return take values
}

class Holder {
    values: List<int> = []

    fn init(self, take values: List<int>) {
        self.values = take values
    }
}

@move_only class Packet {
    id: int
}

fn packet_id(take packet: Packet) -> int {
    return packet.id
}

fn swap(inout left: int, inout right: int) {
    let old: int = left
    left = right
    right = old
}

fn replace(inout values: List<int>, take replacement: List<int>) {
    values = take replacement
}

interface Editor {
    fn edit(self, inout value: int)
}

class AddOne : Editor {
    override fn edit(self, inout value: int) { value += 1 }
}

fn main() {
    var item: Item = make(3)
    let moved: Item = take item
    io.println("move {read(moved)}")

    item = make(5)
    if item.value > 0 {
        let left: Item = take item
        io.println("left {left.value}")
    } else {
        let right: Item = take item
        io.println("right {right.value}")
    }
    item = make(7)

    var kept: Item = make(9)
    if false {
        let gone: Item = take kept
        io.println("gone {gone.value}")
    }
    kept = make(10)

    var emptied: Item = make(12)
    if true {
        let gone: Item = take emptied
        io.println("gone {gone.value}")
    }
    emptied = make(14)

    var number: int = 11
    let old: int = take number
    number = 13
    io.println("again {item.value} {kept.value} {emptied.value} {old} {number}")

    var numbers: List<int> = [3, 1]
    let owned_numbers: List<int> = take numbers
    numbers = [8]
    let copied_numbers: List<int> = owned_numbers.clone()
    io.println("{owned_numbers} {copied_numbers} {numbers}")

    var labels: Map<string, int> = {"a": 1}
    let owned_labels: Map<string, int> = take labels
    labels = {"b": 2}
    let copied_labels: Map<string, int> = owned_labels.clone()
    io.println("{owned_labels.get("a").or(0)} {copied_labels.len()} {labels.contains("b")}")

    var payload: List<int> = [1, 2]
    let returned: List<int> = round_trip(take payload)
    var held_values: List<int> = [8, 9]
    let holder: Holder = Holder(take held_values)
    io.println("{returned} {holder.values.len()}")

    var packet: Packet = Packet { id: 42 }
    io.println(packet_id(take packet))

    var left: int = 4
    var right: int = 9
    swap(inout left, inout right)
    let editor: Editor = AddOne {}
    editor.edit(inout left)
    var current: List<int> = [1]
    var replacement: List<int> = [7, 8]
    replace(inout current, take replacement)
    io.println("{left} {right} {current}")
}
