fn main() {
    let first: Box<int> = Box.new(1)
    let copied: Box<int> = first
    let second: Box<int> = Box.new(2)
    var values: List<Box<int>> = []
    values.push(second)
    let wrapped: Option<Box<int>> = some(second)
    let shared: Shared<Box<int>> = Shared.new(second)
}
