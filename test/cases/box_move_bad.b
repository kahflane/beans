fn main() {
    let first: Box<int> = new Box(1)
    let copied: Box<int> = first
    let second: Box<int> = new Box(2)
    var values: List<Box<int>> = []
    values.push(second)
    let wrapped: Option<Box<int>> = some(second)
    let shared: Shared<Box<int>> = new Shared(second)
}
