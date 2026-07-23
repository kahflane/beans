struct Pair {
    left: i32
    right: i32
}

fn main() {
    let values: Arena<Pair> = Arena.new(1)
    values.put(Pair { left: 1, right: 2 })
    values.at(3)
}
