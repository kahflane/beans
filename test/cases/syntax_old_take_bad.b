class Item {}
fn consume(take item: Item) {}
fn main() {
    let item: Item = new Item()
    let moved: Item = take item
}
