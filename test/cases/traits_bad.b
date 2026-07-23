import std.thread

class Local {
    value: int = 7
}

fn unknown<T implements Magic>(value: T) -> T { return value }
fn needs_order<T implements Order>(value: T) -> T { return value }

fn missing_clone<T>(values: List<T>) -> List<T> {
    return values.clone()
}

fn main() {
    let local: Local = new Local()
    let wrong: Local = needs_order(local)
    let worker: Thread<int> = thread.spawn(fn() -> int { return local.value })
    let shared: Shared<Local> = new Shared(local)
    let worker2: Thread<int> = thread.spawn(fn() -> int { return shared.get().value })
    let worker3: Thread<Local> = thread.spawn(fn() -> Local { return new Local() })
    let bad_map: Map<Shared<int>, int> = {}
}
