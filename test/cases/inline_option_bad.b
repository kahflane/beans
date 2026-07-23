import std.thread

struct Pair {
    left: i32
    right: i32
}

enum Wrapper {
    item(value: Pair)
}

fn old_slots<T>(value: T) -> List<T> {
    return [value]
}

fn main() {
    let in_list: List<Option<Pair>> = []
    let bypass: List<Pair> = old_slots(Pair { left: 1, right: 2 })
    let worker: Thread<Option<Pair>> = thread.spawn(fn() -> Option<Pair> {
        return none
    })
    in_list.len()
    bypass.len()
    worker.join()
}
