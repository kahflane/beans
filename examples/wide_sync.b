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
}

fn share<T>(value: T) -> Shared<T> {
    return Shared.new(value)
}

fn guard<T>(value: T) -> Mutex<T> {
    return Mutex.new(value)
}

fn dead_event() -> Weak<Event> {
    let shared: Shared<Event> = share(Event { label: "short", value: 3 })
    let weak: Weak<Event> = shared.downgrade()
    let value: Event = shared.get()
    io.println("weak live {value.label} {weak.expired()}")
    return weak
}

fn make_mutex_cycle() {
    let target: Box<Option<MutexOwner>> = Box.new(none)
    let mutex: Mutex<MutexEdge> = Mutex.new(MutexEdge { target: take target })
    let owner: MutexOwner = MutexOwner { guard: mutex }
    owner.guard.with(fn(edge: MutexEdge) {
        edge.target.set(some(owner))
    })
}

fn main() {
    let shared: Shared<Event> = share(Event { label: "worker", value: 42 })
    let copy: Event = shared.get()
    io.println("shared {copy.label} {copy.value}")

    let array: Shared<[i64; 2]> = Shared.new([7, 8])
    let numbers: [i64; 2] = array.get()
    let amount: Shared<decimal> = Shared.new(19.99)
    io.println("shared values {numbers[0]} {numbers[1]} {amount.get() + 0.01}")

    let weak: Weak<Event> = dead_event()
    io.println("weak dead {weak.expired()} {weak.upgrade().is_none()}")

    let mutex: Mutex<Event> = guard(Event { label: "locked", value: 9 })
    mutex.with(fn(value: Event) {
        io.println("mutex {value.label} {value.value}")
    })

    let decimal_mutex: Mutex<decimal> = Mutex.new(2.50)
    decimal_mutex.with(fn(value: decimal) {
        io.println("mutex decimal {value + 0.25}")
    })

    let result_mutex: Mutex<Result<Pair>> = Mutex.new(err("guarded error"))
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
