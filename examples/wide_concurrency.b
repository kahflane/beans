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

struct ChannelEdge {
    target: Box<Option<ChannelOwner>>
}

class ChannelOwner {
    messages: Channel<ChannelEdge>
}

fn send_one<T>(channel: Channel<T>, value: T) {
    channel.send(value)
}

fn make_channel_cycle() {
    let target: Box<Option<ChannelOwner>> = Box.new(none)
    let messages: Channel<ChannelEdge> = Channel.new(1)
    let owner: ChannelOwner = ChannelOwner { messages: messages }
    target.set(some(owner))
    messages.send(ChannelEdge { target: take target })
}

fn main() {
    let messages: Channel<Event> = Channel.new(1)
    let sender: Thread<[i64; 2]> = thread.spawn(fn() -> [i64; 2] {
        send_one(messages, Event { label: "from worker", value: 41 })
        return [7, 8]
    })
    let event: Event = messages.recv().expect("event")
    let result: [i64; 2] = sender.join()
    io.println("channel {event.label} {event.value}")
    io.println("thread array {result[0]} {result[1]}")
    messages.close()
    io.println("channel closed {messages.recv().is_none()}")

    let integers: Channel<int> = Channel.new(1)
    integers.send(12)
    let integer: int = integers.recv().or(-1)
    io.println("channel int {integer}")

    let decimals: Channel<decimal> = Channel.new(2)
    decimals.send(1.25)
    decimals.send(2.50)
    io.println("channel decimal {decimals.recv().or(0.0) + decimals.recv().or(0.0)}")

    let guarded: Channel<Result<Pair>> = Channel.new(1)
    guarded.send(err("channel error"))
    match guarded.recv().expect("result") {
        ok(pair) => { io.println("bad {pair.left}") },
        err(error) => { io.println("channel result {error.msg}") },
    }

    let event_thread: Thread<Event> = thread.spawn(fn() -> Event {
        return Event { label: "thread event", value: 9 }
    })
    let thread_event: Event = event_thread.join()
    io.println("thread event {thread_event.label} {thread_event.value}")

    let decimal_thread: Thread<decimal> = thread.spawn(fn() -> decimal {
        return 3.75
    })
    io.println("thread decimal {decimal_thread.join()}")

    let option_thread: Thread<Option<Pair>> = thread.spawn(fn() -> Option<Pair> {
        return some(Pair { left: 5, right: 6 })
    })
    match option_thread.join() {
        some(pair) => { io.println("thread option {pair.left} {pair.right}") },
        none => { io.println("bad option") },
    }

    let string_thread: Thread<string> = thread.spawn(fn() -> string {
        return "owned string"
    })
    io.println("thread string {string_thread.join()}")

    make_channel_cycle()
}
