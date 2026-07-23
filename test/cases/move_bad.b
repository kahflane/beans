class Item {
    value: int = 0
}

fn use_after_move() {
    let item: Item = Item { value: 1 }
    let moved: Item = take item
    let bad: int = item.value
}

fn maybe_move(flag: bool) {
    let item: Item = Item { value: 2 }
    if flag {
        let moved: Item = take item
    }
    let bad: int = item.value
}

fn borrowed_parameter(item: Item) {
    let bad: Item = take item
}

fn loop_move() {
    let item: Item = Item { value: 3 }
    for true {
        let bad: Item = take item
        break
    }
}

fn captured_move() {
    let item: Item = Item { value: 4 }
    let reader: fn() -> int = fn() -> int { return item.value }
    let bad: Item = take item
}

fn field_move() {
    let item: Item = Item { value: 5 }
    let bad: int = take item.value
}

fn main() { }
