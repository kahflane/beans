import std.io
import std.thread

struct Event {
    label: string
    value: int
}

struct Pair {
    left: i32
    right: i32
}

struct MutexEdge {
    target: Box<Option<MutexOwner>>
}

class MutexOwner {
    guard: Mutex<MutexEdge>

    fn init(guard: Mutex<MutexEdge>) { self.guard = guard }
}

fn share<T>(value: T) -> Shared<T> {
    return new Shared(value)
}

fn guard<T>(value: T) -> Mutex<T> {
    return new Mutex(value)
}

fn dead_event() -> Weak<Event> {
    let shared: Shared<Event> = share(Event { label: "short", value: 3 })
    let weak: Weak<Event> = shared.downgrade()
    let value: Event = shared.get()
    io.println("weak live {value.label} {weak.expired()}")
    return weak
}

fn make_mutex_cycle() {
    let target: Box<Option<MutexOwner>> = new Box(none)
    let mutex: Mutex<MutexEdge> = new Mutex(MutexEdge { target: move target })
    let owner: MutexOwner = new MutexOwner(mutex)
    owner.guard.with(fn(edge: MutexEdge) {
        edge.target.set(some(owner))
    })
}

fn main() {
    let shared: Shared<Event> = share(Event { label: "worker", value: 42 })
    let copy: Event = shared.get()
    io.println("shared {copy.label} {copy.value}")

    let array: Shared<[i64; 2]> = new Shared([7, 8])
    let numbers: [i64; 2] = array.get()
    let amount: Shared<decimal> = new Shared(19.99)
    io.println("shared values {numbers[0]} {numbers[1]} {amount.get() + 0.01}")

    let weak: Weak<Event> = dead_event()
    io.println("weak dead {weak.expired()} {weak.upgrade().is_none()}")

    let mutex: Mutex<Event> = guard(Event { label: "locked", value: 9 })
    mutex.with(fn(value: Event) {
        io.println("mutex {value.label} {value.value}")
    })

    let decimal_mutex: Mutex<decimal> = new Mutex(2.50)
    decimal_mutex.with(fn(value: decimal) {
        io.println("mutex decimal {value + 0.25}")
    })

    let result_mutex: Mutex<Result<Pair>> = new Mutex(err("guarded error"))
    result_mutex.with(fn(value: Result<Pair>) {
        match value {
            ok(pair) => { io.println("bad {pair.left}") },
            err(error) => { io.println("mutex result {error.msg}") },
        }
    })

    let worker: Thread<int> = thread.spawn(fn() -> int {
        let event: Event = shared.get()
        return event.value + 1
    })
    io.println("thread {worker.join()}")

    make_mutex_cycle()
}
