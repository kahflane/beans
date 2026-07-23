import std.io
import std.thread

interface Greeter {
    fn name() -> string
    fn greet() -> string {
        return "hi {self.name()}"
    }
}

class Counter implements Greeter {
    count: int = 0
    pub label: string

    fn init(label: string) {
        self.label = label
    }

    fn name() -> string {
        return self.label
    }

    override fn greet() -> string {
        return "counter {self.label} at {self.count}"
    }

    fn bump(by: int) {
        self.count += by
    }
}

class Stack<T> {
    items: List<T> = []

    fn push(x: T) { self.items.push(x) }
    fn pop() -> Option<T> { return self.items.pop() }
    fn size() -> int { return self.items.len() }
}

enum Payment {
    cash
    card(number: string)
    transfer(iban: string, amount: decimal)

    fn label() -> string {
        return match self {
            cash => "cash",
            card(n) => "card {n.last(4)}",
            transfer(iban, amt) => "wire {amt} to {iban}",
        }
    }
}

fn largest<T implements Order>(xs: List<T>) -> Option<T> {
    return xs.max()
}

fn parse_qty(s: string) -> Result<int> {
    let n: int = s.to_int()?
    if n < 0 {
        return err("quantity can't be negative")
    }
    return ok(n)
}

fn main() {
    // decimal money math, no implicit conversions
    let price: decimal = 19.99
    let qty: int = 3
    let total: decimal = price * (qty as decimal)
    io.println("total {total}")

    // generic class
    var st: Stack<int> = new Stack()
    st.push(1)
    st.push(2)
    io.println(st.pop().or(-1))
    io.println(st.size())

    // generic fn with inference
    let best: int = largest([3, 1, 4]).or(0)
    io.println(best)

    // enums + match exhaustiveness
    let p: Payment = Payment.transfer("DE89370400440532013000", 250.00)
    io.println(p.label())
    let p2: Payment = Payment.cash
    io.println(p2.label())

    // option / result
    match parse_qty("42") {
        ok(n)  => io.println("qty {n}"),
        err(e) => io.println("bad: {e.msg}"),
    }

    // inheritance, interfaces, upcast, downcast
    let g: Greeter = new Counter("jobs")
    io.println(g.greet())
    match g as? Counter {
        some(c) => c.bump(1),
        none    => io.println("plain greeter"),
    }

    // collections
    var m: Map<string, int> = {"a": 1, "b": 2}
    m.set("c", 3)
    io.println(m.get("a").or(0))
    let names: List<string> = ["mia", "jul"]
    for nm: string in names {
        io.println(nm)
    }

    // threads, mutex, channel
    let t: Thread<int> = thread.spawn(fn() -> int {
        return 21 * 2
    })
    io.println(t.join())

    let shared: Mutex<Counter> = new Mutex(new Counter("shared"))
    shared.with(fn(c: Counter) {
        c.bump(5)
    })

    let ch: Channel<string> = new Channel(8)
    ch.send("job")
    defer ch.close()

    // value forms
    let grade: string = if best >= 4 { "big" } else { "small" }
    let kind: string = match best {
        0       => "zero",
        1 | 2   => "couple",
        3..=9   => "few",
        _       => "many",
    }
    io.println("{grade} {kind}")

    var i: int = 0
    for i < 3 {
        i += 1
    }
    for x: int in 0..=2 {
        io.println(x)
    }
}
