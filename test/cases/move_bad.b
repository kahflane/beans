class Item {
    value: int = 0

    fn init(value: int) { self.value = value }
}

fn use_after_move() {
    let item: Item = new Item(1)
    let moved: Item = move item
    let bad: int = item.value
}

fn maybe_move(flag: bool) {
    let item: Item = new Item(2)
    if flag {
        let moved: Item = move item
    }
    let bad: int = item.value
}

fn borrowed_parameter(item: Item) {
    let bad: Item = move item
}

fn loop_move() {
    let item: Item = new Item(3)
    for true {
        let bad: Item = move item
        break
    }
}

fn captured_move() {
    let item: Item = new Item(4)
    let reader: fn() -> int = fn() -> int { return item.value }
    let bad: Item = move item
}

fn field_move() {
    let item: Item = new Item(5)
    let bad: int = move item.value
}

fn main() { }
