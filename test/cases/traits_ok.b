import std.io

fn same<T: Eq>(a: T, b: T) -> bool {
    return a == b
}

fn sorted_copy<T: Order + Clone>(values: List<T>) -> List<T> {
    let out: List<T> = values.clone()
    out.sort()
    return take out
}

fn main() {
    io.println("{same(7, 7)} {same("a", "b")}")
    let values: List<int> = [4, 1, 3]
    io.println("{sorted_copy(values)} {values}")
}
